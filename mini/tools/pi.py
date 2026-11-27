#!/usr/bin/env python3
"""Talk to an Orange Pi Zero3 running BoltOS Mini, over its USB-C cable.

While BoltOS Mini runs, the Pi is a network adapter on the Mac at 10.77.0.1:

    pi.py screen         set the Pi's clock from the Mac's, then open its
                         screen in Screen Sharing
    pi.py time           just set the clock
    pi.py shot [FILE]    save a screenshot (PNG if Pillow is there, else BMP)
    pi.py log            print BoltOS's log (or, if BoltOS is not running,
                         the one the last run left in RAM, through U-Boot)
    pi.py reboot         restart the Pi
    pi.py firmware FILE  write FILE (BoltBoot, or any eGON boot image) to the
                         card's boot area, 8 KiB in; the files on the card
                         stay as they are (development kernels only)

Every time the Pi starts, U-Boot spends two seconds as a USB DFU device before
it boots the kernel on the card. In that window this tool can also:

    pi.py boot KERNEL    run KERNEL instead of the card's (RAM only, nothing
                         on the card changes) -- the development loop
    pi.py install KERNEL write KERNEL to the card as BOLTOS.BIN, then boot it

Both restart a running BoltOS first, so a new kernel goes over without
anybody touching the Pi.

With no card in it at power-on, the H618's boot ROM waits on USB-C in its
FEL mode instead, and these talk to the ROM itself (mini/boot/, BoltBoot):

    pi.py fel                       which chip, which die
    pi.py fel run boltboot.bin      run boot0 in SRAM and print its report
    pi.py fel read ADDR LEN [FILE]  read memory (hex dump, or into FILE)
 The DFU commands need pyusb and libusb
(build/.tools/env has both; see MINI.md); the others need only Python.
"""
import argparse, os, struct, sys, time, urllib.request

try:
    import usb.core, usb.util
except ImportError:
    usb = None

# BOLTOS_PI=192.168.86.236 reaches it over the LAN instead (Ethernet or Wi-Fi),
# for when the USB-C link is down; the DFU commands still need the cable.
PI = "http://" + os.environ.get("BOLTOS_PI", "10.77.0.1")


def http(path, data=None, timeout=2.0):
    """A request to BoltOS Mini's web server; None if it is not answering."""
    try:
        with urllib.request.urlopen(PI + path, data=data, timeout=timeout) as r:
            return r.read()
    except Exception:
        return None


def running():
    return http("/", timeout=0.7) is not None


def set_time():
    import calendar
    t = int(time.time())
    tz = (calendar.timegm(time.localtime(t)) - t) // 60
    r = http("/time?t=%d&tz=%d" % (t, tz), data=b"")
    print("  the Pi's clock is now %s" % time.strftime("%H:%M") if r else "  could not reach the Pi")
    return r is not None

VID, PID = 0x1F3A, 0x1010          # U-Boot's sunxi download gadget
DFU_DETACH, DFU_DNLOAD, DFU_UPLOAD, DFU_GETSTATUS, DFU_CLRSTATUS, DFU_GETSTATE, DFU_ABORT = range(7)
ST_IDLE, ST_DNLOAD_SYNC, ST_DNBUSY, ST_DNLOAD_IDLE, ST_MANIFEST_SYNC, ST_MANIFEST, \
    ST_MANIFEST_WAIT_RESET, ST_UPLOAD_IDLE, ST_ERROR = 2, 3, 4, 5, 6, 7, 8, 9, 10
LOG_MAGIC = b"BLOG"


class Dfu:
    def __init__(self, dev):
        self.dev = dev
        try:
            dev.set_configuration()
        except usb.core.USBError:
            pass
        cfg = dev.get_active_configuration()
        self.alts = {}
        self.intf = None
        self.xfer = 4096
        for it in cfg:
            if it.bInterfaceClass != 0xFE or it.bInterfaceSubClass != 0x01:
                continue
            self.intf = it.bInterfaceNumber
            name = usb.util.get_string(dev, it.iInterface) if it.iInterface else str(it.bAlternateSetting)
            self.alts[name] = it.bAlternateSetting
            extra = bytes(it.extra_descriptors or b"")
            if len(extra) >= 7 and extra[1] == 0x21:
                self.xfer = extra[5] | (extra[6] << 8)
        if self.intf is None:
            raise SystemExit("the Pi is on USB but shows no DFU interface")
        try:
            if dev.is_kernel_driver_active(self.intf):
                dev.detach_kernel_driver(self.intf)
        except (usb.core.USBError, NotImplementedError):
            pass
        usb.util.claim_interface(dev, self.intf)

    def _out(self, req, value, data=b""):
        return self.dev.ctrl_transfer(0x21, req, value, self.intf, data, timeout=5000)

    def _in(self, req, value, n):
        return bytes(self.dev.ctrl_transfer(0xA1, req, value, self.intf, n, timeout=5000))

    def status(self):
        s = self._in(DFU_GETSTATUS, 0, 6)
        poll = s[1] | (s[2] << 8) | (s[3] << 16)
        return s[0], s[4], poll

    def idle(self):
        st, state, _ = self.status()
        if state == ST_ERROR:
            self._out(DFU_CLRSTATUS, 0)
        elif state != ST_IDLE:
            self._out(DFU_ABORT, 0)

    def select(self, name):
        if name not in self.alts:
            raise SystemExit("the Pi offers %s, not %r" % (sorted(self.alts), name))
        self.dev.set_interface_altsetting(interface=self.intf,
                                          alternate_setting=self.alts[name])
        self.idle()

    def download(self, name, data, what):
        self.select(name)
        n = (len(data) + self.xfer - 1) // self.xfer
        t0 = time.time()
        for blk in range(n):
            self._out(DFU_DNLOAD, blk, data[blk * self.xfer:(blk + 1) * self.xfer])
            while True:
                st, state, poll = self.status()
                if st != 0:
                    raise SystemExit("the Pi refused block %d (DFU status %d)" % (blk, st))
                if state == ST_DNLOAD_IDLE:
                    break
                time.sleep(poll / 1000.0)
            if blk % 64 == 0 or blk == n - 1:
                pct = 100 * (blk + 1) // n
                sys.stdout.write("\r  %s: %3d%%  (%d KiB)" % (what, pct, len(data) >> 10))
                sys.stdout.flush()
        self._out(DFU_DNLOAD, n, b"")                     # end of file: manifest
        while True:
            st, state, poll = self.status()
            if st != 0:
                raise SystemExit("\nthe Pi could not store %s (DFU status %d)" % (what, st))
            if state in (ST_IDLE, ST_MANIFEST_WAIT_RESET):
                break
            time.sleep(max(poll, 10) / 1000.0)
        dt = time.time() - t0
        print("\r  %s: done, %d KiB in %.1f s" % (what, len(data) >> 10, dt))

    def upload(self, name, limit):
        self.select(name)
        out = bytearray()
        blk = 0
        while len(out) < limit:
            chunk = self._in(DFU_UPLOAD, blk, self.xfer)
            out += chunk
            blk += 1
            if len(chunk) < self.xfer:
                break
            if blk == 1 and out[:4] != LOG_MAGIC:
                break                                    # nothing was logged here
            if blk == 1:
                limit = min(limit, 16 + int.from_bytes(out[4:8], "little"))
        self.idle()
        return bytes(out)

    def detach(self):
        try:
            self._out(DFU_DETACH, 1000)
        except usb.core.USBError:
            pass
        usb.util.release_interface(self.dev, self.intf)
        usb.util.dispose_resources(self.dev)


def ask_reboot():
    if http("/reboot", data=b"") is not None:
        print("  asked BoltOS Mini to restart")
        return True
    return False


def wait_for_pi(timeout):
    if usb is None:
        raise SystemExit("pyusb is missing: run this with build/.tools/env/bin/python3, "
                         "or pip install pyusb and brew install libusb")
    t0 = time.time()
    said = False
    while True:
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is not None:
            return Dfu(dev)
        if not said:
            print("Waiting for the Pi's bootloader on USB-C (plug the Pi in, or reset it)...")
            said = True
        if timeout and time.time() - t0 > timeout:
            raise SystemExit("no Pi after %d s" % timeout)
        time.sleep(0.02)


def print_log(raw):
    if raw[:4] != LOG_MAGIC:
        print("  (no BoltOS log in RAM: the Pi was off, or nothing has run yet)")
        return
    n = int.from_bytes(raw[4:8], "little")
    boots = int.from_bytes(raw[8:12], "little")
    text = raw[16:16 + n].decode("utf-8", "replace")
    print("---- log from the last run (boot #%d, %d bytes) ----" % (boots, n))
    sys.stdout.write(text if text.endswith("\n") else text + "\n")
    print("---- end of log ----")


# ---- the boot ROM's FEL mode ------------------------------------------------
# With no card to boot (or no valid one), the H618's ROM is a USB device that
# can read and write memory and call code: two bulk endpoints, every message
# wrapped as "AWUC" request -> data -> "AWUS" reply, and on top of that a
# 16-byte FEL request (version, write, exec, read) answered by an 8-byte status.
FEL_PID = 0xEFE8
FEL_VERSION, FEL_WRITE, FEL_EXEC, FEL_READ = 0x001, 0x101, 0x102, 0x103
FEL_CHUNK = 64 * 1024
SRAM, FEL_HOLE, FEL_HOLE_END = 0x20000, 0x21000, 0x22000      # mini/boot/boot.h
BOOT0_REPORT, THUNK = 0x50000, 0x53A00
H616_SOC = 0x1823


class FelError(Exception):
    pass


class Fel:
    def __init__(self, dev):
        self.dev = dev
        try:
            cfg = dev.get_active_configuration()
        except usb.core.USBError:
            dev.set_configuration()
            cfg = dev.get_active_configuration()
        it = cfg[(0, 0)]
        try:
            if dev.is_kernel_driver_active(0):
                dev.detach_kernel_driver(0)
        except (usb.core.USBError, NotImplementedError):
            pass
        usb.util.claim_interface(dev, 0)
        self.ep_out = usb.util.find_descriptor(it, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.ep_in = usb.util.find_descriptor(it, custom_match=lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

    def _usb(self, kind, length):
        req = b"AWUC\0\0\0\0" + struct.pack("<IIHI", length, 0x0C000000, kind, length) + bytes(10)
        self.ep_out.write(req, 10000)

    def _reply(self, timeout=10000):
        r = bytes(self.ep_in.read(64, timeout))
        if r[:4] != b"AWUS":
            raise FelError("the boot ROM answered %s, not AWUS" % r.hex())

    def _write(self, data, timeout=10000):
        self._usb(0x12, len(data))
        self.ep_out.write(data, timeout)
        self._reply()

    def _read(self, n, timeout=10000):
        self._usb(0x11, n)
        out = bytearray()
        while len(out) < n:
            out += self.ep_in.read(n - len(out), timeout)
        self._reply()
        return bytes(out)

    def _request(self, kind, addr, length, timeout=10000):
        self._write(struct.pack("<IIII", kind, addr, length, 0), timeout)

    def _status(self, timeout=10000):
        self._read(8, timeout)

    def version(self):
        self._request(FEL_VERSION, 0, 0)
        v = self._read(32)
        self._status()
        return {"sig": v[:8], "soc": (struct.unpack_from("<I", v, 8)[0] >> 8) & 0xFFFF,
                "protocol": struct.unpack_from("<H", v, 16)[0],
                "scratch": struct.unpack_from("<I", v, 20)[0]}

    def read(self, addr, n):
        out = bytearray()
        while len(out) < n:
            k = min(FEL_CHUNK, n - len(out))
            self._request(FEL_READ, addr + len(out), k)
            out += self._read(k)
            self._status()
        return bytes(out)

    def write(self, addr, data):
        for o in range(0, len(data), FEL_CHUNK):
            k = data[o:o + FEL_CHUNK]
            self._request(FEL_WRITE, addr + o, len(k))
            self._write(k)
            self._status()

    def exec(self, addr, timeout=10000):
        """Call ARM32 code at addr; back when it returns to the ROM."""
        self._request(FEL_EXEC, addr, 0)
        self._status(timeout)

    def r32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def icache_flush(self):
        # ICIALLU; DSB; ISB; BX LR -- so code just written is what runs.
        self.write(THUNK, struct.pack("<4I", 0xEE070F15, 0xF57FF04F, 0xF57FF06F, 0xE12FFF1E))
        self.exec(THUNK)


def wait_for_fel(timeout):
    if usb is None:
        raise SystemExit("pyusb is missing: run this with build/.tools/env/bin/python3")
    t0 = time.time()
    said = False
    while True:
        dev = usb.core.find(idVendor=VID, idProduct=FEL_PID)
        if dev is not None:
            time.sleep(0.5)                 # let it finish enumerating
            try:
                fel = Fel(dev)
                v = fel.version()
            except (FelError, usb.core.USBError) as e:
                print("  FEL handshake failed (%s); trying again" % e)
                time.sleep(1)
                continue
            if v["sig"] != b"AWUSBFEX":
                raise SystemExit("a FEL device that says %r" % v["sig"])
            return fel, v
        if not said:
            print("Waiting for the Pi's boot ROM in FEL mode (take the card out, "
                  "plug USB-C into the Mac)...")
            said = True
        if timeout and time.time() - t0 > timeout:
            raise SystemExit("no FEL device after %d s" % timeout)
        time.sleep(0.1)


def fel_report(fel):
    head = fel.read(BOOT0_REPORT, 8)
    if head[:4] != b"BRPT":
        print("  (no report from boot0 at 0x%x)" % BOOT0_REPORT)
        return
    n = min(struct.unpack_from("<I", head, 4)[0], 4096 - 8)
    text = fel.read(BOOT0_REPORT + 8, (n + 3) & ~3)[:n].decode("utf-8", "replace")
    print("---- boot0 ----")
    sys.stdout.write(text if text.endswith("\n") else text + "\n")
    print("---- end ----")


def fel_main(args, timeout):
    fel, v = wait_for_fel(timeout)
    soc = "H616/H618" if v["soc"] == H616_SOC else "an unknown SoC"
    if not args:
        die = fel.r32(0x03000024) & 0xFF
        print("FEL: SoC %04x (%s), protocol %d, scratch 0x%x, SRAM_VER die %d (RVBAR at 0x%08x)"
              % (v["soc"], soc, v["protocol"], v["scratch"], die, 0x08100040 if die else 0x09010040))
        return
    if v["soc"] != H616_SOC:
        raise SystemExit("this is SoC %04x, not the Zero3's H618" % v["soc"])
    what = args[0]
    if what == "run":
        if len(args) < 2:
            raise SystemExit("usage: pi.py fel run build/mini/boltboot.bin")
        img = bytearray(open(args[1], "rb").read())
        if img[4:12] != b"eGON.BT0" or img[0x14:0x18] != b"BOLT":
            raise SystemExit("%s is not BoltBoot" % args[1])
        n = struct.unpack_from("<I", img, 0x10)[0]
        boot0 = img[:n]
        struct.pack_into("<I", boot0, 0x18, 1)               # flags: from FEL, come back
        fel.write(BOOT0_REPORT, b"\0" * 8)
        # Everything but the 4 KiB the ROM keeps its stacks in, while it runs.
        fel.write(SRAM, bytes(boot0[:FEL_HOLE - SRAM]))
        fel.write(FEL_HOLE_END, bytes(boot0[FEL_HOLE_END - SRAM:]))
        fel.icache_flush()
        print("  boot0: %d KiB into SRAM, running it" % (n >> 10))
        t0 = time.time()
        fel.exec(SRAM, timeout=60000)
        print("  boot0 came back after %.2f s" % (time.time() - t0))
        fel_report(fel)
    elif what == "read":
        if len(args) < 3:
            raise SystemExit("usage: pi.py fel read ADDR LEN [FILE]")
        addr, n = int(args[1], 0), int(args[2], 0)
        data = fel.read(addr, n)
        if len(args) > 3:
            open(args[3], "wb").write(data)
            print("  %d bytes from 0x%x into %s" % (n, addr, args[3]))
        else:
            for o in range(0, n, 16):
                row = data[o:o + 16]
                print("%08x  %s" % (addr + o, " ".join("%08x" % w for w in
                      struct.unpack("<%dI" % (len(row) // 4), row[:len(row) // 4 * 4]))))
    else:
        raise SystemExit("pi.py fel [run FILE | read ADDR LEN [FILE]]")



def put_firmware(path):
    """Hand a boot image to a running (development) BoltOS Mini, which writes
    it to the card's boot area, 8 KiB in, and reads it back. The partition
    with the files is not touched. The Pi starts it at the next power-on."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkboot
    img = open(path, "rb").read()
    if not mkboot.check(img):
        raise SystemExit("%s is not a boot image the H618's ROM would run (eGON header or sum)" % path)
    if len(img) > mkboot.GAP:
        raise SystemExit("%s is too big for the space in front of the partition" % path)
    if not running():
        raise SystemExit("BoltOS Mini did not answer at %s" % PI[7:])
    r = http("/firmware", data=img, timeout=60)
    if r is None:
        raise SystemExit("no answer from /firmware (is this a development kernel?)")
    sys.stdout.write(r.decode("utf-8", "replace"))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cmd", choices=["screen", "time", "shot", "log", "reboot", "boot", "install", "wait",
                                    "fel", "firmware"])
    ap.add_argument("kernel", nargs="?", help="the kernel (boot, install) or file name (shot)")
    ap.add_argument("more", nargs="*", help=argparse.SUPPRESS)
    ap.add_argument("--no-log", action="store_true", help="do not fetch the last log")
    ap.add_argument("--timeout", type=int, default=0)
    a = ap.parse_args(argv)

    if a.cmd == "fel":
        fel_main(([a.kernel] if a.kernel else []) + a.more, a.timeout)
        return
    if a.cmd == "firmware":
        if not a.kernel:
            ap.error("firmware needs a boot image (build/mini/boltboot.bin)")
        put_firmware(a.kernel)
        return
    if a.cmd in ("screen", "time"):
        for _ in range(60):
            if running():
                break
            if _ == 0:
                print("Waiting for BoltOS Mini at %s (is the Pi plugged into this Mac?)..." % PI[7:])
            time.sleep(1)
        else:
            raise SystemExit("BoltOS Mini did not answer at %s" % PI[7:])
        set_time()
        if a.cmd == "screen":
            import subprocess
            print("  opening vnc://10.77.0.1 in Screen Sharing (password: boltos)")
            subprocess.run(["open", "vnc://:boltos@10.77.0.1"])
        return
    if a.cmd == "shot":
        out = a.kernel or "boltos-mini.png"
        bmp = http("/screen.bmp", timeout=20)
        if not bmp:
            raise SystemExit("BoltOS Mini did not answer at %s" % PI[7:])
        try:
            import io
            from PIL import Image
            Image.open(io.BytesIO(bmp)).save(out)
        except ImportError:
            out = out.rsplit(".", 1)[0] + ".bmp"
            open(out, "wb").write(bmp)
        print("saved %s" % out)
        return
    if a.cmd == "reboot":
        if not ask_reboot():
            raise SystemExit("BoltOS Mini did not answer at %s" % PI[7:])
        return
    if a.cmd == "log" and running():
        sys.stdout.write((http("/log", timeout=5) or b"").decode("utf-8", "replace"))
        return
    if a.cmd in ("boot", "install") and not a.kernel:
        ap.error("%s needs a kernel file (build/mini/boltos.bin)" % a.cmd)

    data = open(a.kernel, "rb").read() if a.kernel else b""
    if data and data[0x38:0x3C] != b"ARM\x64":
        raise SystemExit("%s is not an ARM64 kernel image" % a.kernel)
    if a.cmd in ("boot", "install") and running():
        ask_reboot()

    pi = wait_for_pi(a.timeout)
    print("Found the Pi (U-Boot DFU: %s)" % ", ".join(sorted(pi.alts)))
    if a.cmd == "log" or (not a.no_log and a.cmd == "boot"):
        print_log(pi.upload("log", 1 << 20))
    if a.cmd == "boot":
        pi.download("kernel", data, "kernel to RAM")
    elif a.cmd == "install":
        pi.download("boltos.bin", data, "BOLTOS.BIN on the card")
    pi.detach()
    if a.cmd in ("boot", "install"):
        print("The Pi is starting BoltOS Mini.")


if __name__ == "__main__":
    main(sys.argv[1:])
