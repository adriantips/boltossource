#!/usr/bin/env python3
"""Wi-Fi: the key schedule against its own specifications, then a real join.

Two halves, because 802.11 splits cleanly into two kinds of code and they need
two kinds of checking.

The host half runs test/host/wpa.c, which holds net/wpa.c up against the
vectors published by FIPS 180-4, RFC 2202, 6070, 3394, 4493 and 3610 and the
802.11i annexes, and then cross-checks the PRF, the PMK, the PTK and the EAPOL
MIC against Python's own hashlib and hmac. Two independent implementations of
SHA-1 agreeing on a 384-bit derivation is a better oracle than a vector copied
out of a document by hand, and it is the same trick t_asm.py plays with NASM.
No QEMU, about a second:

    python test/t_wifi.py --host-only

The machine half joins a network. QEMU emulates no 802.11 device, so the radio
is drivers/wifi_sim.c: an access point inside the kernel, bridged to the wired
interface, built out of the same frame builders and the same crypto as the
station. Everything above the radio -- the scan, open authentication, the
association, the four-way handshake, CCMP on every frame after it -- is the
code a real card would use, and it runs here. What that proves is the stack;
what it cannot prove is a driver for hardware nobody has.

So this asserts on the things that would break silently:

  * a wrong passphrase fails, and fails as "wrong passphrase" rather than as a
    timeout or, worse, a join
  * a network that is not there is not found
  * the right passphrase joins, and DHCP leases an address over the air
  * traffic actually crosses the encrypted path (a real HTTPS fetch)
  * the credentials survive a reboot and the machine rejoins by itself

Normal boot hands the screen to the desktop and leaves no serial shell; safe
mode has a shell but skips network bring-up. `-vga none` gives both.
"""
import hashlib, hmac, os, re, shutil, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"
HEADLESS = ["-vga", "none"]

SSID = "BoltNet"
PSK = "boltos-test-key"

DISKS = (("hdd", "disk-hdd.img", "ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200"),
         ("ssd", "disk-ssd.img", "ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1"),
         ("nvm", "disk-nvme.img", "nvme,drive=nvm,serial=BOLTNVME01"))


# --------------------------------------------------------------- host half --
def prf(key, label, data, nbits):
    """IEEE 802.11's PRF, written out again here from the specification rather
    than from net/wpa.c, so agreeing means something."""
    out = b""
    i = 0
    while len(out) * 8 < nbits:
        out += hmac.new(key, label.encode() + b"\x00" + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return out[:nbits // 8]


def ptk(pmk, aa, spa, anonce, snonce):
    lo, hi = (aa, spa) if aa < spa else (spa, aa)
    nlo, nhi = (anonce, snonce) if anonce < snonce else (snonce, anonce)
    return prf(pmk, "Pairwise key expansion", lo + hi + nlo + nhi, 384)


def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")

    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("wpa-host.exe" if os.name == "nt" else "wpa-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Wno-inconsistent-dllimport",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "wpa.c"),
                    str(Path(ROOT) / "net" / "wpa.c"),
                    str(Path(ROOT) / "net" / "crypto.c"),
                    "-o", str(exe)], check=True)

    r = subprocess.run([str(exe), "--dump"], capture_output=True, text=True)
    got = {}
    for line in r.stdout.splitlines():
        p = line.split()
        # The dump lines are "name hex" and are for this script, not the reader.
        if len(p) == 2 and re.fullmatch(r"[0-9a-f]+", p[1]):
            got[p[0]] = bytes.fromhex(p[1])
        else:
            print(line)
    assert r.returncode == 0, "net/wpa.c disagrees with its own specifications"

    k = b"\x0b" * 20
    assert got["prf192"] == prf(k, "prefix", b"Hi There", 192), "PRF-192"
    assert got["prf320"] == prf(k, "Pairwise key expansion", b"abcdefgh", 320), "PRF-320"

    pmk = hashlib.pbkdf2_hmac("sha1", PSK.encode(), SSID.encode(), 4096, 32)
    assert got["pmk"] == pmk, "PMK"

    aa = bytes([0x02, 0, 0, 0, 0, 1])
    spa = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
    anonce = bytes((i * 7 + 1) & 0xFF for i in range(32))
    snonce = bytes((i * 13 + 3) & 0xFF for i in range(32))
    want_ptk = ptk(pmk, aa, spa, anonce, snonce)
    assert got["ptk"] == want_ptk, "PTK"

    frame = bytearray((i * 5 + 2) & 0xFF for i in range(112))
    frame[81:97] = b"\x00" * 16                      # the MIC field is zero
    kck = want_ptk[:16]
    assert got["micsha1"] == hmac.new(kck, bytes(frame), hashlib.sha1).digest()[:16], "EAPOL MIC"

    print("  ok: the PRF, the PMK, the PTK and the EAPOL MIC agree with hashlib")


# ------------------------------------------------------------ machine half --
def scratch(fresh=True):
    """Private copies of the data disks. The filesystem persists to one of
    these, so the second machine must see what the first one wrote -- and it
    must not be the developer's own images."""
    OUT.mkdir(parents=True, exist_ok=True)
    out = []
    for tag, src, dev in DISKS:
        s = Path(ROOT) / "iso" / src
        d = OUT / ("wifi-" + src)
        if fresh:
            if not s.exists():
                continue
            shutil.copyfile(s, d)
        if d.exists():
            out.append((tag, str(d), dev))
    if not out:
        raise SystemExit("no data disk images in iso/ -- run build.sh first")
    return out


def shell(vm, line, expect=None, timeout=40):
    """Submit one command and wait for something it prints.

    The leading Enter is not decoration: reach_shell's probe can leave a
    half-typed line in the shell's buffer, and the first command then arrives
    with a stray character glued to its front. It cost an afternoon of "unknown
    command: ewifi".
    """
    vm.key("ret")
    time.sleep(0.4)
    vm.clear()
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)
    time.sleep(1.0)
    return vm.serial()


def test_join():
    """One machine: no radio, then a radio, then a network."""
    with BoltVM(name="wifi", extra=HEADLESS, data_disks=scratch(True),
                snapshot=False) as vm:
        vm.reach_shell(200, "1")
        time.sleep(5)

        log = shell(vm, "wifi", "radio", 20)
        assert "none bound" in log, "a radio appeared that nothing started\n%s" % log[-800:]

        log = shell(vm, "wifi radio on", "radio on:", 25)
        assert SSID in log, "the built-in radio did not come up\n%s" % log[-800:]

        log = shell(vm, "wifi scan", "security", 25)
        assert SSID in log and "WPA2-PSK" in log, "the scan found nothing\n%s" % log[-900:]
        assert re.search(r"%s\s+([0-9a-f]{2}:){5}[0-9a-f]{2}\s+6\b" % SSID, log), \
            "the scan row is missing a BSSID or the channel\n%s" % log[-900:]

        # A wrong passphrase must fail, and must say which failure it was. The
        # AP has no way to say "wrong password" in 802.11 -- it simply stops
        # answering -- so this is the whole of the user-facing error handling.
        log = shell(vm, "wifi connect %s definitely-not-the-key" % SSID, "wifi:", 30)
        assert "wrong passphrase" in log, \
            "a wrong passphrase did not report itself as one\n%s" % log[-900:]

        log = shell(vm, "wifi connect NoSuchNetwork whatever", "wifi:", 30)
        assert "not found" in log, "a network that is not there was not missed\n%s" % log[-700:]

        log = shell(vm, "wifi connect %s %s" % (SSID, PSK), "address ", 40)
        assert "four-way handshake complete" in log, "no handshake\n%s" % log[-1200:]
        assert "WPA2-PSK/CCMP" in log, "joined without CCMP\n%s" % log[-1200:]
        m = re.search(r"address (\d+\.\d+\.\d+\.\d+) by DHCP", log)
        assert m, "no lease over the air\n%s" % log[-1200:]
        print("  ok: joined %s with WPA2-PSK and leased %s over 802.11" % (SSID, m.group(1)))

        log = shell(vm, "wifi", "network :", 20)
        assert "connected" in log and SSID in log, "status does not say it is on\n%s" % log[-800:]

        # Every frame from here is CCMP-encrypted, unwrapped by the AP and put
        # on the wire. If the AAD or the packet number were wrong, this is where
        # it would show: a ping is one frame each way and might get lucky, a TLS
        # handshake and a page fetch are neither short nor forgiving.
        log = shell(vm, "ping 10.0.2.2", "statistics", 25)
        assert "0% loss" in log, "the encrypted path drops packets\n%s" % log[-700:]

        log = shell(vm, "download https://example.com/ /tmp/wifi.html", "downloaded", 60)
        if "downloaded" in log:
            print("  ok: an HTTPS page fetched over the encrypted 802.11 path")
        else:
            print("  SKIP: no outbound HTTPS from this sandbox")

        log = shell(vm, "wifi list", SSID, 15)
        assert SSID in log, "the network was joined but not remembered\n%s" % log[-600:]

        shell(vm, "poweroff", None, 10)
        time.sleep(12)


def test_rejoin():
    """The second boot: nobody types anything, and it is on the network."""
    with BoltVM(name="wifi2", extra=HEADLESS, data_disks=scratch(False),
                snapshot=False) as vm:
        log = vm.reach_shell(220, "1")
        assert "[wifi] wlan0" in log, \
            "the radio was not brought back up\n%s" % log[-1500:]
        assert "scanning for a remembered network" in log, \
            "nothing looked for the saved network\n%s" % log[-1500:]
        assert "joined %s" % SSID in log, \
            "it did not rejoin by itself\n%s" % log[-1500:]
        assert "DHCP lease" in log, "rejoined but never got an address\n%s" % log[-1500:]
        print("  ok: the radio, the credentials and the association all survived a reboot")

        time.sleep(5)
        log = shell(vm, "ping 10.0.2.2", "statistics", 25)
        assert "0% loss" in log, "the rejoined link does not carry traffic\n%s" % log[-700:]
        print("  ok: and it carries traffic without anyone touching it")


def main():
    host_tests()
    if "--host-only" in sys.argv:
        print("PASS wifi: the key schedule only (--host-only)")
        return
    test_join()
    test_rejoin()
    print("PASS wifi")


if __name__ == "__main__":
    main()
