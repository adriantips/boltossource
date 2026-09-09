#!/usr/bin/env python3
"""Filesystem durability: files survive a reboot, and survive a power cut.

Runs against private copies of the data disks (snapshot=False, so writes really
land) and drives the guest from the text shell.

  1. write a file, `sync`, then `reboot`  -> the file is still there, and the
     clean shutdown means the next boot does NOT need to fsck
  2. write a second file, then KILL QEMU outright (no shutdown at all)
     -> the next boot notices the unclean shutdown, fscks, and both files are
        intact with the image checksum passing

Step 2 is the one that matters. The old in-place image rewrite had no atomic
commit at all: a power cut during a save left a half-written image and lost the
entire filesystem, not just the file being written.

The test boots via the boot menu's safe mode, which gives a text shell on the
console instead of the desktop. That keeps it off the GUI login screen and lets
it read every command's output straight from COM1.
"""
import os, shutil, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = os.path.join(ROOT, "test", "out")
MARK1 = "durability-marker-one"
MARK2 = "durability-marker-two"

DISKS = (("hdd", "disk-hdd.img", "ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200"),
         ("ssd", "disk-ssd.img", "ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1"),
         ("nvm", "disk-nvme.img", "nvme,drive=nvm,serial=BOLTNVME01"))


def scratch_disks(fresh=True):
    """Private copies of the data disks, so the run is repeatable and does not
    disturb the developer's persistent images."""
    os.makedirs(OUT, exist_ok=True)
    out = []
    for tag, src, dev in DISKS:
        s = os.path.join(ROOT, "iso", src)
        d = os.path.join(OUT, "persist-" + src)
        if fresh:
            if not os.path.exists(s):
                continue
            shutil.copyfile(s, d)
        if os.path.exists(d):
            out.append((tag, d, dev))
    if not out:
        raise SystemExit("no data disk images in iso/ -- run build.sh first")
    return out


def reach_shell(vm, timeout=180):
    """Pick safe mode in the boot menu, then wait for the text shell prompt."""
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    vm.wait_prompt()


def shell(vm, line, expect=None, timeout=60):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def cat(vm, path, want, timeout=60):
    """Read a file and assert on THIS command's output.

    Not vm.expect(): the serial log carries the echo of every line typed, so
    waiting for a marker after typing `write FILE marker` matches the echo and
    passes whatever the filesystem actually did. Asserting on the output of a
    `cat` that has run through to its next prompt is the only form of this
    check that can fail.
    """
    out = vm.run("cat " + path, timeout=timeout)
    assert want in out.split(path, 1)[-1], (
        "%s did not read back as expected:\n%s" % (path, out))
    return out


def main():
    vm = BoltVM(name="persist", snapshot=False, data_disks=scratch_disks(), net=False)
    vm.start()
    try:
        reach_shell(vm)
        if "no data disk found" in vm.serial():
            print("SKIP persist: the guest found no persistent disk")
            return 0

        # --- 1. write + sync + a clean reboot ---------------------------------
        vm.run("write /home/dur1.txt " + MARK1)
        shell(vm, "sync", "sync: committed FS")

        vm.clear()
        shell(vm, "reboot", "BoltFS committed and marked clean")
        reach_shell(vm)
        vm.expect("fs: loaded image", timeout=40)
        # A clean shutdown must not cost the next boot a filesystem check.
        vm.expect_absent("last shutdown was unclean")
        cat(vm, "/home/dur1.txt", MARK1)
        print("  ok: file survived a clean reboot, and the image stayed clean")

        # --- 2. write, then yank the power ------------------------------------
        # vm.run waits for the prompt, so autosave has really committed the
        # write before the plug is pulled. Typing it and killing QEMU in the
        # same breath measures how fast the host is, not whether BoltFS is safe.
        vm.run("write /home/dur2.txt " + MARK2)
        cat(vm, "/home/dur2.txt", MARK2)
        # No sync and no shutdown: autosave has committed it, and the machine now
        # dies with no chance to flush anything.
        vm.power_cycle()
        reach_shell(vm)

        log = vm.serial()
        assert "last shutdown was unclean" in log, \
            "the FS did not notice the unclean shutdown:\n" + log[-2000:]
        assert "fsck: " in log, "no fsck ran after the unclean shutdown"
        print("  ok: unclean shutdown detected and checked at mount")

        cat(vm, "/home/dur1.txt", MARK1)
        cat(vm, "/home/dur2.txt", MARK2)
        print("  ok: both files intact after the power cut")

        shell(vm, "fsck", "structure   : ok")
        assert "checksum    : ok" in vm.serial(), "blob checksum failed after the power cut"
        print("  ok: fsck reports a healthy image")

        shell(vm, "poweroff", "BoltFS committed and marked clean")
        time.sleep(2.0)
    finally:
        vm.stop()

    print("PASS persist: survives reboot and power loss with no data lost")
    return 0


if __name__ == "__main__":
    sys.exit(main())
