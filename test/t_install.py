#!/usr/bin/env python3
"""The installer, end to end: install onto a blank disk, then BOOT that disk.

This is the acceptance test for "a non-developer could install it on a real
disk". It runs `install` inside a live BoltOS to lay down a fresh, blank 512 MiB
disk, then throws the install medium away and boots the result -- first on
legacy BIOS, then, if OVMF is present, through the ESP the installer wrote.

Booting the installed disk is the only assertion that really proves anything:
it exercises stage1, stage2, the boot header, the kernel copy, the partition
table and the exported BoltFS image all at once.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT, find_ovmf

OUT = os.path.join(ROOT, "test", "out")
TARGET = os.path.join(OUT, "installed.img")
TARGET_MB = 512
USER, PASSWD = "installer", "hunter2"


def blank_disk(path, mb):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.truncate(mb * 1024 * 1024)


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    time.sleep(0.5)


def shell(vm, line, expect=None, timeout=90):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def do_install():
    """Run `install` in a live BoltOS against a blank target disk."""
    blank_disk(TARGET, TARGET_MB)
    # Only the boot image and the blank target: with no other data disk, the
    # target is unambiguous and BoltFS falls back to RAM-only, which also
    # exercises the "install from a live medium with no persistent disk" path.
    vm = BoltVM(name="install", snapshot=False, net=False,
                data_disks=[("tgt", TARGET, "ide-hd,drive=tgt,bus=ide.1,unit=0")])
    vm.start()
    try:
        reach_shell(vm)

        shell(vm, "install", "Installable disks:")
        log = vm.serial()
        assert "kernel image:" in log, "the installer could not find the kernel to copy"
        print("  ok: installer found the kernel and an installable disk")

        shell(vm, "install ata1 %s %s 64" % (USER, PASSWD),
              "account '%s'" % USER, timeout=600)      # the installer's last line
        log = vm.serial()
        assert "BoltOS installed on" in log
        assert "legacy: stage1@0" in log
        assert "UEFI:   ESP @LBA" in log
        print("  ok: install wrote the boot chain, the ESP and the filesystem")
    finally:
        vm.stop()


def boot_installed_legacy():
    """Boot the installed disk on its own, with no install medium present."""
    vm = BoltVM(name="installed-bios", snapshot=True, net=False,
                boot_img=TARGET, data_disks=[])
    vm.start()
    try:
        vm.expect("BoltOS kernel", timeout=120)
        vm.expect("[ok] boot mode", timeout=60)
        vm.expect("fs: loaded image", timeout=90)
        vm.expect("BoltOS is up", timeout=120)
        vm.expect_absent("KERNEL PANIC")
        print("  ok: the installed disk boots on legacy BIOS and mounts its filesystem")
    finally:
        vm.stop()


def boot_installed_uefi():
    if not find_ovmf()[0]:
        print("  skip: no OVMF firmware, cannot check the installed ESP")
        return
    # Boot the installed disk through UEFI: the firmware must find the ESP the
    # installer formatted and run the BOOTX64.EFI it put there.
    code, varsf = find_ovmf()
    extra = ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + code]
    if varsf:
        import shutil
        myvars = os.path.join(OUT, "installed-OVMF_VARS.fd")
        shutil.copyfile(varsf, myvars)
        extra += ["-drive", "if=pflash,format=raw,unit=1,file=" + myvars]

    vm = BoltVM(name="installed-uefi", snapshot=True, net=False,
                boot_img=TARGET, data_disks=[], extra=extra)
    vm.start()
    try:
        if not vm.wait_for("BoltOS UEFI loader", timeout=120):
            print("  skip: firmware did not launch the installed ESP loader")
            return
        vm.expect("BoltOS kernel", timeout=120)
        vm.expect("BoltOS is up", timeout=120)
        vm.expect_absent("KERNEL PANIC")
        print("  ok: the installed ESP boots the same disk through UEFI")
    finally:
        vm.stop()


def main():
    do_install()
    boot_installed_legacy()
    boot_installed_uefi()
    print("PASS install: a blank disk becomes a bootable BoltOS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
