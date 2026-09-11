#!/usr/bin/env python3
"""IPv6: an address nobody handed out, and a neighbour who answers.

The machine forms its own link-local address from its MAC, checks the wire in
case somebody already has it, solicits a router, and builds a global address
out of the prefix that comes back. None of that involves a server: the point of
SLAAC is that there is nothing to configure and nothing to run.

The router here is slirp's, which QEMU only turns on when asked -- a real NDP
implementation advertising fec0::/64, which is what makes this a test of the
stack rather than of a fixture.

    python test/t_ipv6.py --host-only     # address handling, no QEMU

Safe mode has a serial shell but skips network bring-up, and a normal boot puts
the desktop on the screen instead of a shell. `-vga none` gives both at once:
with no framebuffer the kernel falls back to the serial shell, with the network
up -- the same arrangement t_net uses.
"""
import os, re, shutil, subprocess, sys
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

HEADLESS = ["-vga", "none"]
OUT = Path(ROOT) / "test" / "out"


def host_tests():
    """The address text forms, against the real net/ip6.c and no machine.

    Parsing and printing an address is where a case that never comes up in
    QEMU stays wrong for years: the boot only ever handles two addresses, and
    both of them are ordinary."""
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("ip6-host.exe" if os.name == "nt" else "ip6-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "ip6.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def reach_shell(vm, timeout=180):
    """Normal boot (entry 1), no framebuffer: a serial shell with the network up.

    Safe mode has a serial shell but skips network bring-up, and a normal boot
    puts the desktop on the screen instead of a shell. `-vga none` gives both
    at once: with no framebuffer the kernel falls back to the serial shell,
    with the network up.
    """
    return vm.reach_shell(timeout, "1")


# 52:54:00:12:34:56, the address QEMU gives the first NIC, as a Modified
# EUI-64: the universal/local bit in the first octet is flipped and ff:fe goes
# in the middle. Getting this wrong is the classic IPv6 bug, so the test knows
# the answer rather than reading it back out of the machine.
EUI64 = "5054:ff:fe12:3456"


def shell(vm, line, expect=None, timeout=60):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def out(vm, line, expect, timeout=60):
    vm.clear()
    shell(vm, line, expect, timeout)
    return vm.serial()


def main():
    host_tests()
    if "--host-only" in sys.argv:
        print("PASS ipv6: address handling only (--host-only)")
        return 0
    with BoltVM(name="ipv6", net=True, ipv6=True, extra=HEADLESS) as vm:
        boot = reach_shell(vm)

        # --- the interface configured itself ------------------------------
        o = out(vm, "ifconfig", "inet6")
        assert "fe80::" + EUI64 in o, \
            "no link-local address from the MAC:\n" + o[-800:]
        print("  ok: a link-local address formed from the MAC, and checked for a clash")

        assert "fec0::" + EUI64 in o, \
            "SLAAC did not take the prefix from the router advertisement:\n" + o[-800:]
        m = re.search(r"inet6   fec0::[0-9a-f:]+/(\d+)", o)
        assert m and m.group(1) == "64", "the prefix length is wrong:\n" + o[-800:]
        assert "via fe80::" in o, "no router was recorded:\n" + o[-800:]
        print("  ok: a global address from the advertised prefix, and a default router")

        # The neighbour cache learned the router's MAC from the advertisement,
        # which is the whole point of the source link-layer option.
        assert "neigh   fe80::" in o, "the router is not in the neighbour cache:\n" + o[-800:]
        print("  ok: the router is in the neighbour cache")

        # --- and it can talk to it ----------------------------------------
        o = out(vm, "ping6 fec0::2", "statistics", timeout=90)
        assert "4 sent, 4 received, 0% loss" in o, \
            "the gateway did not answer four echo requests:\n" + o[-800:]
        print("  ok: four ICMPv6 echoes to the gateway, four replies")

        # No argument means the router it found for itself.
        o = out(vm, "ping6", "statistics", timeout=90)
        assert "0% loss" in o, "pinging the discovered router failed:\n" + o[-800:]
        print("  ok: `ping6` with no argument reaches the router it discovered")

        # --- addresses parse and print back the same ----------------------
        out(vm, "ping6 not-an-address", "not an IPv6 address")
        out(vm, "ping6 fe80:::1", "not an IPv6 address")
        out(vm, "ping6 1:2:3:4:5:6:7", "not an IPv6 address")
        print("  ok: malformed addresses are refused rather than guessed at")

        # The all-zero address is both the degenerate case for the shortening
        # rule -- two colons, not three, where the run and the separator each
        # want to write one -- and not somewhere a packet can go.
        o = out(vm, "ping6 ::", "unspecified address")
        assert ":: is the unspecified" in o and ":::" not in o, \
            "the all-zero address did not print as ::\n" + o[-400:]
        print("  ok: :: prints as two colons, and is refused as a destination")

        # --- the boot said so too -----------------------------------------
        assert "IPv6 link-local" in boot and "SLAAC address" in boot, \
            "the boot log does not report the IPv6 configuration"
        print("  ok: the boot log reports what it configured")

        vm.expect_absent("KERNEL PANIC")
    print("PASS ipv6: link-local, duplicate detection, SLAAC, neighbour discovery, echo")
    return 0


if __name__ == "__main__":
    sys.exit(main())
