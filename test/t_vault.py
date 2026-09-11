#!/usr/bin/env python3
"""An encrypted directory that is really encrypted.

`vault` used to XOR a file against its own password, which is a command called
vault rather than a vault. This drives the replacement: AES-256-GCM over the
whole directory, a key stretched from the password with PBKDF2, and -- the part
worth testing -- the plaintext gone from the disk while it is locked.

Gone from the disk, not from the filesystem: the test runs on private copies of
the data disks with snapshot=False, so writes really land, and then searches the
image files on the host for the secret. That is the question a stolen machine
asks. It also checks the other half honestly: while the vault is unlocked the
secret IS on the disk, because unlocked means ordinary files.

What the scan cannot be asked after a clean shutdown is whether the secret is
anywhere at all: by then the shell history and the kernel log have been written
out, and both of them saw it typed and printed. That is true of anything typed
at a terminal and is not something the vault can do anything about.
"""
import os, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = os.path.join(ROOT, "test", "out")
SECRET = "the-combination-is-31415"
INNER = "second-secret-file-here"
PASSWORD = "hunter2-vault"

DISKS = (("hdd", "disk-hdd.img", "ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200"),
         ("ssd", "disk-ssd.img", "ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1"),
         ("nvm", "disk-nvme.img", "nvme,drive=nvm,serial=BOLTNVME01"))


def scratch_disks():
    """Private copies, so the run is repeatable and leaves the developer's
    images alone. Same arrangement as t_persist."""
    os.makedirs(OUT, exist_ok=True)
    out = []
    for tag, src, dev in DISKS:
        s = os.path.join(ROOT, "iso", src)
        if not os.path.exists(s):
            continue
        d = os.path.join(OUT, "vault-" + src)
        shutil.copyfile(s, d)
        out.append((tag, d, dev))
    if not out:
        raise SystemExit("no data disk images in iso/ -- run build.sh first")
    return out


def on_disk(disks, text):
    """Is `text` anywhere in the raw bytes of any data disk?

    The tail of each chunk is carried into the next, so a secret that straddles
    a read boundary is still found -- a scan that can miss is worse than none
    here, because missing is what passing looks like.
    """
    needle = text.encode()
    for _tag, path, _dev in disks:
        with open(path, "rb") as fh:
            tail = b""
            while True:
                chunk = fh.read(4 << 20)
                if not chunk:
                    break
                if needle in tail + chunk:
                    return True
                tail = chunk[-len(needle):]
    return False


def shell(vm, line, expect=None, timeout=60):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def out(vm, line, expect, timeout=60):
    vm.clear()
    shell(vm, line, expect, timeout)
    return vm.serial()


def main():
    disks = scratch_disks()
    with BoltVM(name="vault", net=False, snapshot=False, data_disks=disks) as vm:
        vm.reach_shell(180, "2")
        shell(vm, "users su user bolt", "now acting as 'user'")

        # --- a vault is made and filled -----------------------------------
        shell(vm, "vault create " + PASSWORD, "vault created")
        shell(vm, "write /home/user/vault/secret.txt " + SECRET, None)
        shell(vm, "mkdir /home/user/vault/inner")
        shell(vm, "write /home/user/vault/inner/more.txt " + INNER, None)
        shell(vm, "cat /home/user/vault/secret.txt", SECRET)
        o = out(vm, "vault status", "vault:")
        assert "unlocked" in o, o[-400:]
        shell(vm, "sync", "committed")
        assert on_disk(disks, SECRET), \
            "an unlocked vault should be ordinary files, and they should be on the disk"
        print("  ok: a vault is created unlocked, and unlocked really does mean plain files")

        # --- locking takes it off the disk --------------------------------
        shell(vm, "vault lock", "vault locked")
        o = out(vm, "vault status", "vault:")
        assert "locked" in o and "unlocked" not in o, o[-400:]
        o = out(vm, "ls /home/user/vault", "")
        assert "secret.txt" not in o, "the plaintext file is still there:\n" + o[-400:]
        o = out(vm, "ls /home/user", ".vault.bv")
        assert not on_disk(disks, SECRET), \
            "the secret is still in the raw bytes of a disk after locking"
        assert not on_disk(disks, INNER), \
            "a file in a subdirectory of the vault survived the lock on disk"
        print("  ok: locking leaves nothing of either file anywhere on the disk")

        # --- a wrong password is refused ----------------------------------
        o = out(vm, "vault unlock definitely-wrong", "vault:")
        assert "wrong password" in o, o[-400:]
        o = out(vm, "vault status", "vault:")
        assert "locked" in o and "unlocked" not in o, \
            "a failed unlock left the vault open:\n" + o[-400:]
        print("  ok: a wrong password is refused, and leaves it locked")

        # --- the right one brings everything back -------------------------
        shell(vm, "vault unlock " + PASSWORD, "vault unlocked")
        shell(vm, "cat /home/user/vault/secret.txt", SECRET)
        shell(vm, "cat /home/user/vault/inner/more.txt", INNER)
        print("  ok: the right password restores the files, subdirectories included")

        # --- shutting down seals it, without asking for the password again
        # This is the one that decides whether the feature is worth having: a
        # person who forgets to lock the vault and turns the machine off should
        # not be handing their files to whoever finds it.
        vm.clear()
        shell(vm, "reboot", "sealed", timeout=90)
        vm.reach_shell(180, "2")
        shell(vm, "users su user bolt", "now acting as 'user'")
        o = out(vm, "vault status", "vault:")
        assert "locked" in o and "unlocked" not in o, \
            "the vault was left open across a shutdown:\n" + o[-400:]
        o = out(vm, "ls /home/user/vault", "")
        assert "secret.txt" not in o and "inner" not in o, \
            "the plaintext files came back across the reboot:\n" + o[-400:]
        o = out(vm, "ls /home/user", ".vault.bv")
        print("  ok: shutting down with the vault open seals it on the way out")

        # The raw-disk scan is deliberately not repeated here, and it is worth
        # being clear why. A clean shutdown writes out this account's shell
        # history and the kernel log, and both of those recorded the secret the
        # moment it was typed at a prompt and printed by `cat`. Neither is in
        # the vault and neither ever was; no amount of sealing can reach a copy
        # that something else wrote down. The scan above ran at the one point
        # where nothing else had -- which is what makes it a test of the vault
        # rather than of the rest of the machine. See include/vault.h.

        # --- and the sealed archive still opens ---------------------------
        shell(vm, "vault unlock " + PASSWORD, "vault unlocked")
        shell(vm, "cat /home/user/vault/secret.txt", SECRET)
        shell(vm, "cat /home/user/vault/inner/more.txt", INNER)
        print("  ok: the archive survived the reboot and still opens")

        vm.expect_absent("KERNEL PANIC")
    print("PASS vault: AES-256-GCM sealing takes the plaintext off the disk and gives it back")
    return 0


if __name__ == "__main__":
    sys.exit(main())
