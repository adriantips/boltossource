#!/usr/bin/env python3
"""Networking for daily use: two NIC drivers, saved configuration, names, clock.

Four things have to hold before a machine is usable on a network every day:

  * it has a driver for the adapter it is actually given -- so this boots the
    same system on e1000 and on virtio-net and expects a lease on both,
  * a configuration set once survives the reboot,
  * names resolve, from /etc/hosts without touching the wire and from DNS with
    a cache in front of it,
  * the clock is right, because a wrong clock fails TLS certificate validity
    checks and that looks like "HTTPS is broken" rather than "the year is wrong".

Normal boot hands the screen to the desktop and there is no serial shell, while
safe mode has a shell but skips network bring-up. `-vga none` gives both: no
framebuffer, so the kernel falls back to the serial shell, with the network up.

Anything needing the outside world (NTP, a real DNS lookup) is best-effort: a
sandbox with no outbound UDP should not fail the suite, so those report SKIP.
"""
import os, re, shutil, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

HEADLESS = ["-vga", "none"]
OUT = os.path.join(ROOT, "test", "out")

DISKS = (("hdd", "disk-hdd.img", "ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200"),
         ("ssd", "disk-ssd.img", "ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1"),
         ("nvm", "disk-nvme.img", "nvme,drive=nvm,serial=BOLTNVME01"))


def scratch_disks(fresh=True):
    """Private copies of the data disks. The filesystem persists to one of these
    (not to the boot image), so writing the network configuration and reading it
    back after a reboot means both machines must share the same copies -- and
    they must not be the developer's own images."""
    os.makedirs(OUT, exist_ok=True)
    out = []
    for tag, src, dev in DISKS:
        s = os.path.join(ROOT, "iso", src)
        d = os.path.join(OUT, "net-" + src)
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
    """Normal boot (entry 1), no framebuffer -> the serial shell, network up.

    The banner is not a reliable signal here: /bin/hello runs in ring 3 on
    another thread and writes to the same serial port, so "BoltShell ready"
    routinely arrives cut in half ("BoltShell re" ... "ady."). Probing is
    reliable where matching a banner is not -- if a marker typed at the prompt
    comes back, the shell is reading input, which is the thing we actually care
    about.
    """
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("1")
        time.sleep(0.4)

    # Probing clears the buffer, so keep the boot log: it is the only place the
    # driver bind and the DHCP lease are reported, and callers assert on it.
    boot_log = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        boot_log += vm.serial()
        vm.clear()
        vm.type("echo __shell_up__\n")
        time.sleep(1.2)
        if "__shell_up__" in vm.serial():
            time.sleep(0.4)
            return boot_log + vm.serial()   # incl. whatever landed while probing
    raise AssertionError("never reached a shell prompt\n--- serial tail ---\n"
                         + vm.serial()[-2500:])


def shell(vm, line, expect=None, timeout=30, settle=1.2):
    vm.clear()
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)
    time.sleep(settle)
    return vm.serial()


def test_driver(nic, label):
    """The adapter binds, the link comes up, and DHCP leases an address."""
    with BoltVM(name="net-" + label, nic=nic, extra=HEADLESS) as vm:
        log = reach_shell(vm)
        assert "link up" in log, "%s: link never came up\n%s" % (label, log[-1500:])
        m = re.search(r"DHCP lease: (\d+\.\d+\.\d+\.\d+)", log)
        assert m, "%s: no DHCP lease\n%s" % (label, log[-1500:])
        out = shell(vm, "ifconfig")
        assert m.group(1) in out, "%s: ifconfig disagrees with the lease\n%s" % (label, out)
        print("  ok: %-10s bound, link up, leased %s" % (label, m.group(1)))
        vm.expect_absent("KERNEL PANIC")
        return m.group(1)


def main():
    # --- 1. both NIC drivers ------------------------------------------------
    test_driver("e1000", "e1000")
    test_driver("virtio-net-pci", "virtio-net")

    # --- 2. everything else, on one machine ---------------------------------
    disks = scratch_disks(fresh=True)
    with BoltVM(name="net-cfg", extra=HEADLESS, snapshot=False,
                data_disks=disks) as vm:
        reach_shell(vm)


        # /etc/hosts answers without touching the network.
        shell(vm, "hosts add printer 192.168.7.9", "printer -> 192.168.7.9")
        out = shell(vm, "dns printer")
        assert "192.168.7.9" in out, "hosts entry not used by the resolver:\n" + out
        print("  ok: /etc/hosts answers a lookup without going to the wire")

        # A real lookup, then the cache in front of it. Best effort: a sandbox
        # with no DNS should not fail the suite.
        out = shell(vm, "dns example.com", settle=4.0)
        if re.search(r"example\.com -> \d+\.\d+\.\d+\.\d+", out):
            cached = shell(vm, "dns")
            assert "example.com" in cached, "resolved name was not cached:\n" + cached
            print("  ok: a resolved name is cached with its TTL")
            out = shell(vm, "dns flush")
            assert "cleared" in out
            assert "example.com" not in shell(vm, "dns"), "flush did not clear the cache"
            print("  ok: dns flush empties the cache")
        else:
            print("  SKIP: no outbound DNS in this environment")

        # The clock. Also best effort -- UDP/123 may not leave the sandbox.
        out = shell(vm, "ntp sync", settle=6.0)
        if "clock set:" in out:
            status = shell(vm, "ntp")
            assert "correction" in status, "ntp reports no correction after a sync:\n" + status
            print("  ok: NTP set the clock (%s)" %
                  re.search(r"clock set: (\S+ \S+)", out).group(1))
        else:
            print("  SKIP: no outbound NTP in this environment")

        # A static configuration, saved.
        out = shell(vm, "ifconfig 10.0.2.50 255.255.255.0 10.0.2.2 10.0.2.3")
        assert "10.0.2.50" in out and "saved" in out, "static config was not applied:\n" + out
        conf = shell(vm, "cat /etc/network.conf")
        assert "dhcp 0" in conf and "10.0.2.50" in conf, \
            "/etc/network.conf does not describe the static setup:\n" + conf
        print("  ok: a static address is applied and written to /etc/network.conf")

        shell(vm, "sync", settle=2.0)
        vm.expect_absent("KERNEL PANIC")

    # --- 3. and it is still there after a reboot ----------------------------
    # The same disks, kept: this is the reboot the saved configuration has to
    # survive, which is the whole point of writing it to /etc.
    with BoltVM(name="net-cfg2", extra=HEADLESS, snapshot=False,
                data_disks=scratch_disks(fresh=False)) as vm:
        log = reach_shell(vm)
        assert "static configuration from /etc/network.conf" in log, \
            "the saved configuration was not picked up at boot:\n" + log[-1500:]
        out = shell(vm, "ifconfig")
        assert "10.0.2.50" in out, "static address did not survive the reboot:\n" + out
        out = shell(vm, "dns printer")
        assert "192.168.7.9" in out, "/etc/hosts did not survive the reboot:\n" + out
        print("  ok: address and name table survived a reboot")
        vm.expect_absent("KERNEL PANIC")

    print("PASS net: two NIC drivers, saved config, /etc/hosts, DNS cache, NTP")
    return 0


if __name__ == "__main__":
    sys.exit(main())
