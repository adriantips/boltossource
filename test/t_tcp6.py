#!/usr/bin/env python3
"""TCP over IPv6: two machines on one wire, and nobody configured either.

The point of this test is what is *not* in it. There is no DHCP server, no
router, no static address, no name -- nothing on the wire but two BoltOS
machines. Each works out its own address from its own MAC, checks that nobody
else has it, and that is the whole of the setup. Then one serves HTTP on it and
the other fetches a file, which is a three-way handshake, a request, a response
with a body, and an orderly close, every segment of it carrying a checksum over
a pseudo-header that IPv4 lets you skip and IPv6 does not.

slirp cannot do this: each guest gets its own private network and no route to
the other. QEMU's socket netdev can -- one instance listens on a host socket,
the other connects, and the two guests see each other's frames with nothing in
between. Same arrangement as t_sshclient.py.

What it checks:
  * two machines form link-local addresses and reach each other with no
    configuration at all
  * a TCP connection opens over IPv6 and carries a real HTTP exchange
  * the bytes that arrive are the bytes that were served
  * the server reports the peer as an IPv6 address rather than as 0.0.0.0,
    which is what a uint32_t peer field gives you once v6 connections exist
  * a connection to an address nobody holds fails instead of hanging
"""
import os, re, socket, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

HEADLESS = ["-vga", "none"]
PORT = 8080
BODY = "the-bytes-that-crossed-the-wire"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def reach_shell(vm, timeout=180):
    """Normal boot, no framebuffer: a serial shell with the network up."""
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("1")
        time.sleep(0.4)
    deadline = time.time() + timeout
    while time.time() < deadline:
        vm.clear()
        vm.type(" echo __shell_up__\n")
        time.sleep(1.2)
        if "__shell_up__" in vm.serial():
            time.sleep(0.4)
            return
    raise AssertionError("never reached a shell\n--- serial ---\n" + vm.serial()[-2000:])


def settle(vm, quiet=0.8, limit=15):
    """Wait for the machine to go quiet before typing at it. A command sent
    while the last one is still printing interleaves into something the shell
    does not recognise -- see the note in t_sshclient.py, which is where that
    was learnt the expensive way."""
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


def link_local(vm):
    """The address the machine gave itself, read back off its own ifconfig."""
    out = run(vm, "ifconfig", "link-local", timeout=60)
    m = re.search(r"inet6\s+(fe80:[0-9a-f:]+)/64\s+\(link-local\)", out)
    assert m, "no link-local address on the interface:\n" + out[-900:]
    return m.group(1)


def main():
    port = free_port()
    srv = BoltVM(name="tcp6srv", extra=HEADLESS,
                 netdev="socket,id=net0,listen=127.0.0.1:%d" % port,
                 mac="52:54:00:12:34:56")
    cli = BoltVM(name="tcp6cli", extra=HEADLESS,
                 netdev="socket,id=net0,connect=127.0.0.1:%d" % port,
                 mac="52:54:00:12:34:57")
    srv.start()
    try:
        time.sleep(1.0)              # the listener has to be up before the dial
        cli.start()
        try:
            reach_shell(srv)
            reach_shell(cli)

            srv6 = link_local(srv)
            cli6 = link_local(cli)
            assert srv6 != cli6, "two machines claimed the same address: " + srv6
            print("  ok: both machines addressed themselves (%s, %s)" % (srv6, cli6))

            # Neighbour Discovery between two real implementations, rather than
            # against slirp's router. ARP's job, on the other side.
            o = run(cli, "ping6 " + srv6, "statistics", timeout=90)
            assert "0% loss" in o, "the machines cannot reach each other:\n" + o[-800:]
            print("  ok: they reach each other with no configuration at all")

            # A file with known contents, served from a directory of its own.
            run(srv, "mkdir /srv", "", timeout=30)
            run(srv, 'write /srv/hello.txt "%s"' % BODY, "", timeout=30)

            settle(srv)
            srv.clear()
            srv.type("serve %d /srv\n" % PORT)
            srv.expect("[%s]:%d" % (srv6, PORT), timeout=60)
            print("  ok: the server says where it can be reached over v6")

            # The fetch. Everything between here and the assert is TCP over
            # IPv6: SYN, SYN|ACK, the request, the response, the close.
            url = "http://[%s]:%d/hello.txt" % (srv6, PORT)
            o = run(cli, "download %s /tmp/got.txt" % url, "downloaded", timeout=120)
            assert "HTTP 200" in o, "the fetch did not succeed:\n" + o[-900:]
            print("  ok: an HTTP exchange over an IPv6 connection")

            o = run(cli, "cat /tmp/got.txt", BODY, timeout=30)
            print("  ok: the bytes that arrived are the bytes that were served")

            # The server's own log of it. Before the peer became a netaddr this
            # said 0.0.0.0, because a v6 peer has no uint32_t form.
            srv_log = srv.serial()
            assert cli6 in srv_log, \
                "the server did not report its peer as an IPv6 address:\n" + srv_log[-900:]
            print("  ok: the server logged the peer as %s" % cli6)

            srv.type(" \n")          # any key stops it
            srv.expect("stopped after", timeout=60)

            # Nobody holds this one. It should give up on the neighbour
            # lookup rather than sit there until the shell's patience runs out.
            gone = srv6.rsplit(":", 1)[0] + ":dead"
            t0 = time.time()
            o = run(cli, "download http://[%s]:%d/hello.txt /tmp/no.txt" % (gone, PORT),
                    "connection failed", timeout=90)
            took = time.time() - t0
            assert "downloaded" not in o[-400:], (
                "a fetch from an address nobody holds reported success:\n" + o[-900:])
            print("  ok: an unreachable address fails in %.0fs rather than hanging" % took)

            cli.expect_absent("KERNEL PANIC")
            srv.expect_absent("KERNEL PANIC")
        finally:
            cli.stop()
    finally:
        srv.stop()
    print("PASS tcp6: two machines, no configuration, and HTTP over IPv6 between them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
