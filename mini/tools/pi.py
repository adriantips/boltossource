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

Every time the Pi starts, U-Boot spends two seconds as a USB DFU device before
it boots the kernel on the card. In that window this tool can also:

    pi.py boot KERNEL    run KERNEL instead of the card's (RAM only, nothing
                         on the card changes) -- the development loop
    pi.py install KERNEL write KERNEL to the card as BOLTOS.BIN, then boot it

Both restart a running BoltOS first, so a new kernel goes over without
anybody touching the Pi. The DFU commands need pyusb and libusb
(build/.tools/env has both; see MINI.md); the others need only Python.
"""
import argparse, os, sys, time, urllib.request

try:
    import usb.core, usb.util
except ImportError:
    usb = None

PI = "http://10.77.0.1"


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


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cmd", choices=["screen", "time", "shot", "log", "reboot", "boot", "install", "wait"])
    ap.add_argument("kernel", nargs="?", help="the kernel (boot, install) or file name (shot)")
    ap.add_argument("--no-log", action="store_true", help="do not fetch the last log")
    ap.add_argument("--timeout", type=int, default=0)
    a = ap.parse_args(argv)

    if a.cmd in ("screen", "time"):
        for _ in range(60):
            if running():
                break
            if _ == 0:
                print("Waiting for BoltOS Mini at 10.77.0.1 (is the Pi plugged into this Mac?)...")
            time.sleep(1)
        else:
            raise SystemExit("BoltOS Mini did not answer at 10.77.0.1")
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
            raise SystemExit("BoltOS Mini did not answer at 10.77.0.1")
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
            raise SystemExit("BoltOS Mini did not answer at 10.77.0.1")
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
