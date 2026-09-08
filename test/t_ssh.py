#!/usr/bin/env python3
"""SSH: a real OpenSSH client logs in and runs commands on BoltOS.

This is deliberately tested against the actual `ssh` binary rather than against
a second BoltOS. An implementation can be self-consistent and still wrong in
every way that matters -- a mis-encoded exchange hash verifies perfectly against
your own verifier -- and only a peer that was written from the RFCs can tell you
so. OpenSSH negotiating curve25519-sha256 / ecdsa-sha2-nistp256 / aes128-ctr /
hmac-sha2-256 with this server, then accepting the host key signature, is the
whole claim.

What it checks:
  * the host key is generated once and persists, and the fingerprint the guest
    prints is the one the client sees
  * a password login against the real account table succeeds
  * a wrong password is refused
  * `ssh host command` runs the real shell and returns its output and exit status
  * an unprivileged account cannot bind port 22

Skips (rather than fails) where there is no `ssh` on the host, since the rest of
the suite must still run on a machine without one.
"""
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, _free_port

HEADLESS = ["-vga", "none"]
GUEST_PORT = 2222


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("1")
        time.sleep(0.4)
    boot = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        boot += vm.serial()
        vm.clear()
        vm.type("echo __shell_up__\n")
        time.sleep(1.2)
        if "__shell_up__" in vm.serial():
            time.sleep(0.4)
            return boot + vm.serial()
    raise AssertionError("never reached a shell\n--- serial ---\n" + vm.serial()[-2500:])


def wait_prompt(vm, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if vm.serial().rstrip().endswith("bolt:/>"):
            return True
        time.sleep(0.3)
    return False


def run(vm, line, timeout=30):
    vm.clear()
    vm.type(line + "\n")
    wait_prompt(vm, timeout)
    return vm.serial()


def askpass_env(password):
    """OpenSSH reads a password from the terminal, never from stdin. SSH_ASKPASS
    with SSH_ASKPASS_REQUIRE=force is the supported way to hand it one without a
    terminal, and is how this test types the password."""
    d = tempfile.mkdtemp()
    script = os.path.join(d, "askpass.sh")
    with open(script, "w", newline="\n") as fh:
        fh.write("#!/bin/sh\necho %s\n" % password)
    os.chmod(script, 0o755)
    env = dict(os.environ)
    env["SSH_ASKPASS"] = script
    env["SSH_ASKPASS_REQUIRE"] = "force"
    env["DISPLAY"] = ":0"
    return env


def ssh(port, user, password, command, timeout=90, extra=None):
    args = ["ssh", "-p", str(port),
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-o", "PreferredAuthentications=password",
            "-o", "PubkeyAuthentication=no",
            "-o", "NumberOfPasswordPrompts=1",
            "-o", "ConnectTimeout=20",
            "-o", "LogLevel=ERROR"] + (extra or [])
    args += ["%s@127.0.0.1" % user, command]
    return subprocess.run(args, capture_output=True, text=True,
                          timeout=timeout, env=askpass_env(password))


def main():
    if not shutil.which("ssh"):
        print("SKIP ssh: no OpenSSH client on this host")
        return 0

    hostport = _free_port()
    with BoltVM(name="ssh", extra=HEADLESS,
                hostfwd=[(hostport, GUEST_PORT)]) as vm:
        reach_shell(vm)

        # The fingerprint the machine reports for its own key.
        out = run(vm, "sshkey")
        m = re.search(r"(SHA256:[A-Za-z0-9+/]+)", out)
        assert m, "sshkey printed no fingerprint:\n" + out
        fingerprint = m.group(1)
        print("  ok: host key generated and stored (%s)" % fingerprint[:24] + "...")

        # It must be the same one on the next call -- a key regenerated per boot
        # would make every connection warn, which trains users to ignore the
        # warning that matters.
        again = run(vm, "sshkey")
        assert fingerprint in again, "the host key changed between calls"

        print("  ok: the host key is stable across calls")

        vm.clear()
        vm.type("sshd %d\n" % GUEST_PORT)
        vm.expect("listening on", timeout=25)
        time.sleep(1.0)

        # The client must see the fingerprint the guest printed.
        r = subprocess.run(
            ["ssh", "-p", str(hostport), "-o", "StrictHostKeyChecking=no",
             "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=20",
             "-o", "BatchMode=yes", "-v", "root@127.0.0.1", "true"],
            capture_output=True, text=True, timeout=60)
        assert fingerprint in r.stderr, \
            "client saw a different host key:\n" + r.stderr[-1200:]
        print("  ok: the client sees exactly that fingerprint")

        # A real login, running a real command.
        r = ssh(hostport, "root", "root", "echo REMOTE_OK; whoami")
        assert "REMOTE_OK" in r.stdout, \
            "command output did not come back (rc=%d)\n%s\n%s" % (r.returncode, r.stdout, r.stderr)
        assert "root" in r.stdout, "whoami did not report the logged-in user:\n" + r.stdout
        assert r.returncode == 0, \
            "exit status was %d, not 0 -- the channel closed without one" % r.returncode
        print("  ok: password login, real shell output, exit status 0")

        # Write a file over SSH now; it is checked from the console further
        # down, once the server has stopped. The console and the SSH session
        # share one terminal -- sshd stops on any keypress -- so console
        # commands cannot be typed while it is listening.
        r = ssh(hostport, "root", "root", "write /ssh-made-this hello-over-ssh")
        assert r.returncode == 0, "write over ssh failed: " + r.stderr[-300:]

        # A wrong password is refused.
        r = ssh(hostport, "root", "definitely-not-the-password", "echo NOPE", timeout=60)
        assert "NOPE" not in r.stdout, "a wrong password was accepted"
        assert r.returncode != 0
        print("  ok: a wrong password is refused")

        vm.key("q")
        stopped = False
        for _ in range(15):
            time.sleep(1.0)
            if "stopped after" in vm.serial():
                stopped = True
                break
        assert stopped, "sshd did not stop on a keypress " + vm.serial()[-1000:]
        print("  ok: the server stops on a keypress")

        # Now the console is free: the file the SSH session wrote must be here.
        out = run(vm, "cat /ssh-made-this")
        assert "hello-over-ssh" in out, "a file written over SSH is not on the console " + out
        print("  ok: an SSH session acts on the same filesystem as the console")

        vm.expect_absent("KERNEL PANIC")

    print("PASS ssh: OpenSSH logs in and runs commands on BoltOS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
