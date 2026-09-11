#!/usr/bin/env python3
"""The SSH client: one BoltOS logs in to another and runs commands on it.

Two machines on one wire. QEMU's socket netdev puts them there -- one instance
listens on a host socket, the other connects to it, and the two guests see each
other's frames with nothing in between. slirp cannot do that: each guest gets
its own private network and no route to the other.

What this proves, and what it does not. The server half was checked against a
real OpenSSH in t_ssh.py, which is the test that matters for an implementation
written from RFCs -- a mis-encoded exchange hash verifies perfectly against your
own verifier. So a client that interoperates with *that* server has been checked
against something a third party validated, which is worth a good deal more than
self-consistency, and rather less than being driven by OpenSSH itself. This host
has no sshd to point it at; if one turns up, point it there instead.

What it checks:
  * a machine idle at a shell prompt answers the network at all -- which it did
    not, until the NIC's interrupt line was actually opened
  * a password login against the far machine's real account table
  * `ssh host command` runs the remote shell and brings back its output
  * a wrong password is refused
  * the host key is remembered on first connection, listed, and can be forgotten
  * a host key that has CHANGED stops the connection instead of asking
"""
import os, socket, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

HEADLESS = ["-vga", "none"]
SRV_IP, CLI_IP = "192.168.7.1", "192.168.7.2"
SSH_PORT = 2222


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def reach_shell(vm, timeout=180):
    """Normal boot (entry 1), no framebuffer: a serial shell with the network up.

    Safe mode has a serial shell but skips network bring-up, and a normal boot
    puts the desktop on the screen instead of a shell. `-vga none` gives both
    at once: with no framebuffer the kernel falls back to the serial shell,
    with the network up.
    """
    return vm.reach_shell(timeout, "1")


def settle(vm, quiet=0.8, limit=15):
    """Wait for the machine to stop talking before typing at it.

    reach_shell() returns as soon as its marker comes back, which is not the
    same as the shell being idle: type the next command a moment too early and
    the two lines interleave into one unknown command. That is not theoretical
    -- it produced "__sifconfig 192.168.7.1", which the shell duly did not
    recognise, and the machine then sat on the wrong address for the rest of
    the test. Silence is the one signal that cannot arrive cut in half."""
    end = time.time() + limit
    last = len(vm.serial())
    mark = time.time()
    while time.time() < end:
        time.sleep(0.2)
        n = len(vm.serial())
        if n != last:
            last, mark = n, time.time()
        elif time.time() - mark > quiet:
            return


def run(vm, line, expect=None, timeout=60):
    settle(vm)
    vm.clear()
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)
    return vm.serial()


def main():
    port = free_port()
    srv = BoltVM(name="sshsrv", extra=HEADLESS,
                 netdev="socket,id=net0,listen=127.0.0.1:%d" % port,
                 mac="52:54:00:12:34:56")
    cli = BoltVM(name="sshcli", extra=HEADLESS,
                 netdev="socket,id=net0,connect=127.0.0.1:%d" % port,
                 mac="52:54:00:12:34:57")
    srv.start()
    try:
        # The listener has to be up before anything dials it.
        time.sleep(1.0)
        cli.start()
        try:
            reach_shell(srv)
            reach_shell(cli)

            # Neither machine gets a lease -- there is no DHCP server on a wire
            # with just the two of them -- so both are addressed by hand.
            run(srv, "ifconfig %s 255.255.255.0 %s" % (SRV_IP, SRV_IP), "", timeout=30)
            run(cli, "ifconfig %s 255.255.255.0 %s" % (CLI_IP, CLI_IP), "", timeout=30)

            # Before anything is running on the far machine at all: it is
            # sitting at a shell prompt, and it still answers. That is not a
            # given -- the NIC's receive interrupt was installed but its line
            # was never opened in the IO-APIC, so frames were only ever taken
            # off the card while something was already polling. An idle BoltOS
            # did not answer so much as an ARP request, and nothing could open
            # a connection to it. Two machines on one wire is the only place
            # that is visible, which is why the check lives here.
            o = run(cli, "ping " + SRV_IP, "statistics", timeout=60)
            assert "0% loss" in o, \
                "a machine idle at a prompt did not answer:\n" + o[-600:]
            print("  ok: a machine idle at a prompt answers the network")

            # The far machine serves on a high port, so no privilege is needed
            # and nothing has to be elevated to run the test. It starts before
            # anything is sent to it, which is the realistic order anyway.
            settle(srv)
            srv.clear()
            srv.type("sshd %d\n" % SSH_PORT)
            srv.expect("listening on %s:%d" % (SRV_IP, SSH_PORT), timeout=60)
            print("  ok: the far machine is serving SSH")


            # While it is serving, `ports` says so. It used to answer
            # "network ports: none (no TCP/UDP stack)" whatever was running,
            # which is a worse answer than no command at all -- somebody
            # working out why their server is unreachable reads it and
            # concludes the wrong thing.
            o = run(cli, "ports", "listening:", timeout=30)
            assert "(nothing)" in o, \
                "the client is not serving anything and should say so:\n" + o[-500:]

            # --- the first connection remembers the key --------------------
            o = run(cli, "ssh -p %d -w bolt user@%s id" % (SSH_PORT, SRV_IP),
                    "uid=1000(user)", timeout=120)
            assert "is new" in o and "SHA256:" in o, \
                "the client did not report the new host key:\n" + o[-900:]
            print("  ok: a password login, a remote command, and its output back")

            o = run(cli, "ssh -p %d -w bolt user@%s ports" % (SSH_PORT, SRV_IP),
                    "listening:", timeout=120)
            assert ("tcp  %d" % SSH_PORT) in o, \
                "`ports` on the serving machine does not list its own port:\n" + o[-700:]
            assert "ESTABLISHED" in o, \
                "`ports` does not list the connection it is answering on:\n" + o[-700:]
            print("  ok: `ports` lists the listener and the live connection")

            o = run(cli, "sshkey known", "SHA256:", timeout=30)
            assert SRV_IP in o, "the host was not remembered:\n" + o[-600:]
            print("  ok: the host key was remembered and is listed")

            # --- and does not report it again ------------------------------
            # And a session starts in that account's home, the way signing in
            # anywhere else does.
            o = run(cli, "ssh -p %d -w bolt user@%s pwd" % (SSH_PORT, SRV_IP),
                    "/home/user", timeout=120)
            assert "is new" not in o, "a known host was announced as new:\n" + o[-600:]
            print("  ok: a second connection to the same host says nothing about keys")

            # --- a wrong password is refused -------------------------------
            o = run(cli, "ssh -p %d -w definitely-wrong user@%s id"
                    % (SSH_PORT, SRV_IP), "authentication failed", timeout=120)
            print("  ok: a wrong password is refused")

            # --- a changed key stops the connection ------------------------
            run(cli, "write /etc/ssh/known_hosts \"%s SHA256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""
                % SRV_IP, "", timeout=30)
            o = run(cli, "ssh -p %d -w bolt user@%s id" % (SSH_PORT, SRV_IP),
                    "has CHANGED", timeout=120)
            assert "Refusing to continue" in o, \
                "a changed host key did not stop the connection:\n" + o[-900:]
            assert "uid=" not in o.split("has CHANGED")[-1], \
                "the session continued past a changed host key:\n" + o[-900:]
            print("  ok: a changed host key refuses, rather than asking")

            # --- forgetting one lets a rebuilt machine back in -------------
            o = run(cli, "sshkey forget " + SRV_IP, "forgot", timeout=30)
            o = run(cli, "ssh -p %d -w bolt user@%s id" % (SSH_PORT, SRV_IP),
                    "uid=1000(user)", timeout=120)
            assert "is new" in o, "forgetting a host did not make it new again:\n" + o[-600:]
            print("  ok: forgetting a host lets a rebuilt machine be accepted again")

            cli.expect_absent("KERNEL PANIC")
            srv.expect_absent("KERNEL PANIC")
        finally:
            cli.stop()
    finally:
        srv.stop()
    print("PASS sshclient: one BoltOS logs in to another, and remembers its key")
    return 0


if __name__ == "__main__":
    sys.exit(main())
