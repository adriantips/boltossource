#!/usr/bin/env python3
"""Accounts and permissions actually enforce.

The account table used to be decoration: passwords were an unsalted FNV-1a
checksum, every shell acted with unlimited authority, and files had no owner at
all. This checks the replacement really refuses things:

  * passwords are salted and stretched, and /etc/passwd holds no plaintext
  * a standard user cannot read another user's private file
  * a standard user cannot write into a directory they do not own
  * a standard user cannot add accounts; sudo lets an admin do it
  * ownership and modes survive a reboot
  * wrong passwords are throttled
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def reach_shell(vm, timeout=180):
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


def sudo(vm, line, password, expect=None, timeout=90):
    """Run a sudo command, answering the password prompt if it appears.

    sudo caches authentication for a grace period, so the prompt only shows up
    on the first call -- the test must handle both cases."""
    vm.clear()
    vm.type(line + "\n")
    deadline = time.time() + 15
    while time.time() < deadline:
        s = vm.serial()
        if "password for" in s:
            vm.type(password + "\n")
            break
        if expect and expect in s:
            return
        time.sleep(0.3)
    if expect:
        vm.expect(expect, timeout=timeout)


def main():
    with BoltVM(name="security", net=False) as vm:
        reach_shell(vm)

        # The shell starts with nobody signed in, which is the bootstrap
        # identity (uid -1) -- sign in as the default admin first.
        shell(vm, "users su user bolt", "now acting as 'user'")
        shell(vm, "id", "uid=1000(user)")
        print("  ok: signed in as a normal account")

        # --- passwords are hashed, not stored ------------------------------
        vm.clear()
        shell(vm, "cat /etc/passwd", "user:", timeout=40)
        time.sleep(1.0)
        recs = [l for l in vm.serial().splitlines() if l.startswith(("root:", "user:"))]
        assert recs, "no account records came back from /etc/passwd"
        for r in recs:
            f = r.split(":")
            assert len(f) >= 6, "malformed record: %r" % r
            assert "bolt" not in f[1] and "bolt" not in f[2], \
                "a plaintext password appears in /etc/passwd: %r" % r
            assert len(f[1]) == 32, "salt is not 16 bytes of hex: %r" % f[1]
            assert len(f[2]) == 64, "hash is not a 32-byte digest: %r" % f[2]
        # Two accounts with the SAME password must not share a hash: that is
        # what the per-account salt is for.
        sudo(vm, "sudo useradd alice samepass", "bolt", "added 'alice'")
        sudo(vm, "sudo useradd bob samepass", "bolt", "added 'bob'")
        vm.clear()
        shell(vm, "cat /etc/passwd", "bob:", timeout=40)
        time.sleep(1.0)
        lines = [l for l in vm.serial().splitlines() if l.startswith(("alice:", "bob:"))]
        assert len(lines) == 2, "expected alice and bob in /etc/passwd, got %r" % lines
        a_hash = lines[0].split(":")[2]
        b_hash = lines[1].split(":")[2]
        assert a_hash != b_hash, "identical passwords produced identical hashes (no salt)"
        assert len(a_hash) == 64, "hash is not a 32-byte digest: %r" % a_hash
        print("  ok: passwords are salted and stretched, no plaintext on disk")

        # --- one user cannot read another's private file --------------------
        shell(vm, "users su alice samepass", "now acting as 'alice'")
        shell(vm, "write /home/alice/secret.txt alice-private-data")
        shell(vm, "chmod 600 /home/alice/secret.txt")
        shell(vm, "ls -l /home/alice", "-rw-------", timeout=40)
        print("  ok: chmod and ls -l show real permission bits")

        vm.clear()
        shell(vm, "users su bob samepass", "now acting as 'bob'")
        shell(vm, "cat /home/alice/secret.txt", "permission denied", timeout=40)
        assert "alice-private-data" not in vm.serial(), \
            "bob read alice's 0600 file"
        print("  ok: a standard user cannot read another user's private file")

        # --- nor write into a directory they do not own ---------------------
        vm.clear()
        shell(vm, "write /home/alice/intruder.txt nope", "permission denied", timeout=40)
        print("  ok: a standard user cannot write into another user's home")

        # --- privilege: no account management without admin -----------------
        vm.clear()
        shell(vm, "useradd mallory hunter2", "permission denied", timeout=40)
        print("  ok: a standard user cannot create accounts")

        # --- sudo works for an admin, and drops afterwards ------------------
        vm.clear()
        shell(vm, "users su user bolt", "now acting as 'user'")
        sudo(vm, "sudo id", "bolt", "uid=0")
        vm.clear()
        shell(vm, "id", "uid=1000", timeout=40)
        assert "uid=0" not in vm.serial(), "elevation leaked past the sudo command"
        print("  ok: sudo elevates one command and drops it afterwards")

        # --- throttling -----------------------------------------------------
        vm.clear()
        for _ in range(4):
            shell(vm, "users su alice wrongpass")
            time.sleep(0.3)
        log = vm.serial()
        assert "authentication failed" in log, "a wrong password was accepted"
        assert "too many failures" in log, "repeated failures were not throttled"
        print("  ok: repeated wrong passwords are throttled")

        # --- ownership survives a reboot ------------------------------------
        vm.clear()
        shell(vm, "reboot", "BoltFS committed and marked clean", timeout=60)
        reach_shell(vm)
        vm.clear()
        shell(vm, "ls -l /home/alice", "-rw-------", timeout=40)
        vm.expect("alice", timeout=20)
        print("  ok: ownership and modes survived a reboot")

        vm.expect_absent("KERNEL PANIC")
    print("PASS security: accounts, permissions and sudo all enforce")
    return 0


if __name__ == "__main__":
    sys.exit(main())
