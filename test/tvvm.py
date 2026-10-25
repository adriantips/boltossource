#!/usr/bin/env python3
"""Drive a BoltOS TV guest in QEMU: boot it, press remote keys, take pictures.

    tvvm.py [--uefi] [--img PATH] [--out DIR] SCRIPT...

SCRIPT is a list of steps, run in order:

    wait:SECONDS            let it run
    key:NAME[,NAME...]      press QEMU key names (down, ret, esc, pgup, a, ...)
    type:TEXT               type a string
    shot:NAME               screendump to OUT/NAME.png
    serial:TEXT[:SECONDS]   wait until TEXT appears on the serial log

The serial log is OUT/serial.log. Nothing here is specific to a Mac except the
default QEMU path, which is the one build-tv.sh's toolchain builds.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU_LOCAL = os.path.join(ROOT, "build", ".tools", "qemu", "bin", "qemu-system-x86_64")
QEMU = QEMU_LOCAL if os.path.exists(QEMU_LOCAL) else "qemu-system-x86_64"
FIRMWARE = os.path.join(ROOT, "build", ".tools", "qemu", "share", "qemu", "edk2-x86_64-code.fd")


class VM:
    def __init__(self, img, out, uefi=False, mem=2048, net=True, extra=(), uefi_img=False):
        self.out = out
        os.makedirs(out, exist_ok=True)
        self.qmp_path = os.path.join(out, "qmp.sock")
        if os.path.exists(self.qmp_path):
            os.unlink(self.qmp_path)
        self.serial = os.path.join(out, "serial.log")
        args = [QEMU, "-machine", "q35", "-m", str(mem), "-vga", "std",
                "-display", "none", "-serial", "file:" + self.serial,
                "-qmp", "unix:%s,server,nowait" % self.qmp_path,
                "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd",
                "-audiodev", "none,id=snd", "-no-reboot"]
        if net:
            args += ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]
        if uefi and uefi_img:                   # firmware booting a whole disk image
            args += ["-drive", "if=pflash,format=raw,readonly=on,file=" + FIRMWARE,
                     "-drive", "file=%s,format=raw,if=ide" % img]
        elif uefi:
            esp = os.path.join(ROOT, "build", "tv", "esp")
            args += ["-drive", "if=pflash,format=raw,readonly=on,file=" + FIRMWARE,
                     "-drive", "file=fat:rw:%s,format=raw" % esp]
        elif img.endswith(".iso"):
            args += ["-cdrom", img, "-boot", "d"]
        else:
            args += ["-drive", "file=%s,format=raw,if=ide" % img]
        args += list(extra)
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for _ in range(100):
            if os.path.exists(self.qmp_path):
                break
            time.sleep(0.1)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.qmp_path)
        self.f = self.sock.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP closed")
            r = json.loads(line)
            if "return" in r or "error" in r:
                return r

    def key(self, *names, hold_ms=80):
        for n in names:
            self.cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": hold_ms})
            time.sleep(0.15)

    def type(self, text):
        table = {" ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon", "-": "minus"}
        for ch in text:
            if ch in table:
                q = table[ch]
                if q.startswith("shift-"):
                    self.cmd("send-key", keys=[{"type": "qcode", "data": "shift"},
                                               {"type": "qcode", "data": q[6:]}])
                else:
                    self.key(q)
            elif ch.isupper():
                self.cmd("send-key", keys=[{"type": "qcode", "data": "shift"},
                                           {"type": "qcode", "data": ch.lower()}])
            else:
                self.key(ch)
            time.sleep(0.05)

    def shot(self, name):
        path = os.path.join(self.out, name + ".png")
        r = self.cmd("screendump", filename=path, format="png")
        if "error" in r:                      # a QEMU built without libpng
            ppm = path[:-4] + ".ppm"
            self.cmd("screendump", filename=ppm)
            try:
                from PIL import Image
                time.sleep(0.2)
                Image.open(ppm).save(path)
                os.unlink(ppm)
            except Exception:
                path = ppm
        return path

    def wait_serial(self, text, timeout=60):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                with open(self.serial, "r", errors="replace") as fh:
                    if text in fh.read():
                        return True
            except FileNotFoundError:
                pass
            time.sleep(0.25)
        return False

    def close(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "iso", "boltostv.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "test", "out", "tv"))
    ap.add_argument("--uefi", action="store_true")
    ap.add_argument("--uefi-img", action="store_true", help="UEFI, booting --img rather than the ESP folder")
    ap.add_argument("--mem", type=int, default=2048)
    ap.add_argument("--usb", action="store_true", help="a USB keyboard on an xHCI controller (keys go to it)")
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()
    extra = ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0"] if a.usb else []
    vm = VM(a.img, a.out, uefi=a.uefi or a.uefi_img, mem=a.mem, uefi_img=a.uefi_img, extra=extra)
    ok = True
    try:
        for s in a.steps:
            kind, _, arg = s.partition(":")
            if kind == "wait":
                time.sleep(float(arg))
            elif kind == "key":
                vm.key(*arg.split(","))
            elif kind == "type":
                vm.type(arg)
            elif kind == "shot":
                print("shot", vm.shot(arg))
            elif kind == "serial":
                text, _, t = arg.partition("|")
                got = vm.wait_serial(text, float(t) if t else 60)
                print("serial", repr(text), "found" if got else "NOT FOUND")
                ok &= got
            else:
                sys.exit("unknown step " + s)
    finally:
        vm.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
