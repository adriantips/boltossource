#!/usr/bin/env python3
"""BoltOS QEMU harness: boot a machine, watch the serial log, type at it.

Every regression test in test/ drives the OS through this one class, so the
device set, the QEMU discovery and the serial plumbing live in a single place.
Serial is captured to a file (QEMU's `-serial file:`) and tailed, which behaves
identically on Windows and Linux hosts -- a pipe would need per-platform
non-blocking reads.

    with BoltVM(name="boot") as vm:
        vm.expect("BoltOS is up", timeout=90)
        vm.type("help\\n")
        vm.expect("commands total")
        vm.screenshot("desktop.ppm")

Keyboard and mouse input go through QMP `input-send-event`, so a test can drive
the GUI exactly the way shot.py does.
"""
import json, os, shutil, socket, subprocess, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_qemu():
    env = os.environ.get("BOLTOS_QEMU")
    if env and os.path.exists(env):
        return env
    for c in (r"C:\Program Files\qemu\qemu-system-x86_64.exe",
              r"C:\msys64\ucrt64\bin\qemu-system-x86_64.exe",
              "/usr/bin/qemu-system-x86_64"):
        if os.path.exists(c):
            return c
    found = shutil.which("qemu-system-x86_64")
    if found:
        return found
    raise SystemExit("QEMU not found (set BOLTOS_QEMU)")


def find_ovmf():
    """(code, vars) UEFI firmware images, or (None, None)."""
    env = os.environ.get("BOLTOS_OVMF_CODE")
    cands = []
    if env and os.path.exists(env):
        cands.append(os.path.dirname(env))
    qd = os.path.dirname(find_qemu())
    cands += [os.path.join(qd, "..", "share", "qemu"), os.path.join(qd, "share"),
              "/usr/share/OVMF", "/usr/share/ovmf"]
    for d in cands:
        for f in ("edk2-x86_64-code.fd", "OVMF_CODE.fd", "OVMF_CODE.4m.fd"):
            p = os.path.join(d, f)
            if not os.path.exists(p):
                continue
            for v in ("edk2-i386-vars.fd", "OVMF_VARS.fd", "OVMF_VARS.4m.fd"):
                vp = os.path.join(d, v)
                if os.path.exists(vp):
                    return os.path.abspath(p), os.path.abspath(vp)
            return os.path.abspath(p), None
    return None, None


# Characters that need a named qcode, or a shift, for type().
_QC = {".": "dot", "/": "slash", "-": "minus", " ": "spc", "\t": "tab",
       "=": "equal", ",": "comma", ";": "semicolon", "'": "apostrophe",
       "[": "bracket_left", "]": "bracket_right", "\\": "backslash",
       "`": "grave_accent"}
_SH = {":": "semicolon", "?": "slash", "+": "equal", "*": "8", "(": "9",
       ")": "0", "_": "minus", "!": "1", "@": "2", "#": "3", "$": "4",
       "%": "5", "^": "6", "&": "7", '"': "apostrophe", "<": "comma",
       ">": "dot", "|": "backslash", "~": "grave_accent",
       "{": "bracket_left", "}": "bracket_right"}


def _drive(tag, path, snapshot):
    s = ",snapshot=on" if snapshot else ""
    return "id=%s,file=%s,format=raw,if=none%s" % (tag, path, s)


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class BoltVM:
    """One QEMU instance with a serial log and a QMP control channel.

    snapshot=True (the default) makes every disk copy-on-write for the run, so a
    test can format and scribble without disturbing the developer's persistent
    images. Persistence tests pass snapshot=False and their own scratch copies.
    """

    def __init__(self, name="vm", uefi=False, disks=True, net=True, snapshot=True,
                 memory="2G", smp=2, extra=None, port=None, workdir=None,
                 boot_img=None, data_disks=None):
        self.name = name
        self.uefi = uefi
        self.workdir = workdir or os.path.join(ROOT, "test", "out")
        os.makedirs(self.workdir, exist_ok=True)
        self.serial_path = os.path.join(self.workdir, name + "-serial.log")
        self.port = port or _free_port()
        self._read = 0          # bytes of the serial log already consumed
        self._buf = ""          # everything seen so far, for expect()
        self.proc = None
        self.qmp = None
        self.pos = [512, 384]   # the PS/2 cursor starts centred

        img = boot_img or os.path.join(ROOT, "iso", "os.img")
        if not os.path.exists(img):
            raise SystemExit("%s missing -- run build.sh first" % img)

        a = [find_qemu()]
        if uefi:
            code, varsf = find_ovmf()
            if not code:
                raise SystemExit("OVMF firmware not found; cannot run a UEFI test")
            # A writable copy of the vars store, so each run starts clean.
            a += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + code]
            if varsf:
                myvars = os.path.join(self.workdir, name + "-OVMF_VARS.fd")
                shutil.copyfile(varsf, myvars)
                a += ["-drive", "if=pflash,format=raw,unit=1,file=" + myvars]
            # The ESP is served straight out of esp/ as a virtual FAT volume.
            a += ["-drive", "id=esp,file=fat:rw:" + os.path.join(ROOT, "esp") +
                  ",format=raw,if=none",
                  "-device", "ide-hd,drive=esp,bus=ide.0,unit=0"]
        else:
            a += ["-drive", _drive("boot", img, snapshot),
                  "-device", "ide-hd,drive=boot,bus=ide.0,unit=0"]

        if data_disks is None:
            data_disks = [("hdd",  "disk-hdd.img",  "ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200"),
                          ("ssd",  "disk-ssd.img",  "ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1"),
                          ("nvm",  "disk-nvme.img", "nvme,drive=nvm,serial=BOLTNVME01")] if disks else []
        for tag, fname, dev in data_disks:
            p = fname if os.path.isabs(fname) else os.path.join(ROOT, "iso", fname)
            if os.path.exists(p):
                a += ["-drive", _drive(tag, p, snapshot), "-device", dev]

        a += ["-m", memory, "-smp", str(smp), "-cpu", "max", "-rtc", "base=utc",
              "-vga", "std", "-global", "VGA.vgamem_mb=64",
              "-audiodev", "none,id=snd0", "-device", "AC97,audiodev=snd0"]
        if net:
            a += ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"]
        a += ["-display", "none",
              "-serial", "file:" + self.serial_path,
              "-qmp", "tcp:127.0.0.1:%d,server,nowait" % self.port,
              "-no-reboot"]
        a += extra or []
        self.args = a

    # ---- lifecycle -------------------------------------------------------
    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    def start(self):
        if os.path.exists(self.serial_path):
            os.remove(self.serial_path)
        self.proc = subprocess.Popen(self.args, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE)
        self._connect_qmp()

    def _connect_qmp(self):
        for _ in range(160):
            if self.proc.poll() is not None:
                err = self.proc.stderr.read().decode("utf-8", "replace")
                raise SystemExit("QEMU exited early:\n" + err)
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=2)
                self.qmp = s.makefile("rwb", buffering=0)
                self._rd()                              # greeting
                self._cmd({"execute": "qmp_capabilities"})
                return
            except OSError:
                time.sleep(0.25)
        raise SystemExit("QMP connect failed")

    def stop(self):
        try:
            if self.qmp:
                self._cmd({"execute": "quit"}, want_reply=False)
        except Exception:
            pass
        if self.proc:
            try:
                self.proc.wait(timeout=10)
            except Exception:
                self.proc.kill()

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    # ---- QMP -------------------------------------------------------------
    def _rd(self):
        while True:
            line = self.qmp.readline()
            if not line:
                return None
            d = json.loads(line)
            if "event" in d:                            # ignore async events
                continue
            return d

    def _cmd(self, d, want_reply=True):
        self.qmp.write((json.dumps(d) + "\n").encode())
        return self._rd() if want_reply else None

    # ---- serial ----------------------------------------------------------
    def serial(self):
        """Everything the guest has written to COM1 so far."""
        try:
            with open(self.serial_path, "rb") as fh:
                fh.seek(self._read)
                chunk = fh.read()
                self._read += len(chunk)
        except FileNotFoundError:
            chunk = b""
        if chunk:
            self._buf += chunk.decode("utf-8", "replace")
        return self._buf

    def wait_for(self, needle, timeout=90, poll=0.25):
        """Block until `needle` appears on the serial log. True if it did."""
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.serial():
                return True
            if self.proc.poll() is not None:
                return needle in self.serial()          # died: one last look
            time.sleep(poll)
        return needle in self.serial()

    def expect(self, needle, timeout=90):
        if not self.wait_for(needle, timeout):
            raise AssertionError("timed out waiting for %r\n--- serial tail ---\n%s"
                                 % (needle, self.serial()[-3000:]))

    def expect_absent(self, needle):
        if needle in self.serial():
            raise AssertionError("unexpected %r on the serial log" % needle)

    # ---- input -----------------------------------------------------------
    def _keyev(self, qcode, down):
        return {"type": "key", "data": {"down": down,
                                        "key": {"type": "qcode", "data": qcode}}}

    def key(self, qcode, shift=False):
        ev = []
        if shift:
            ev.append(self._keyev("shift", True))
        ev += [self._keyev(qcode, True), self._keyev(qcode, False)]
        if shift:
            ev.append(self._keyev("shift", False))
        self._cmd({"execute": "input-send-event", "arguments": {"events": ev}})
        time.sleep(0.05)

    def type(self, text, delay=0.0):
        for ch in text:
            if ch == "\n":
                self.key("ret")
            elif ch.isupper():
                self.key(ch.lower(), shift=True)
            elif ch in _SH:
                self.key(_SH[ch], shift=True)
            elif ch in _QC:
                self.key(_QC[ch])
            else:
                self.key(ch)
            if delay:
                time.sleep(delay)

    def move(self, x, y):
        self._cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": x - self.pos[0]}},
            {"type": "rel", "data": {"axis": "y", "value": y - self.pos[1]}}]}})
        self.pos = [x, y]
        time.sleep(0.2)

    def click(self, x, y, double=False):
        self.move(x, y)
        for _ in range(2 if double else 1):
            self._cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"button": "left", "down": True}}]}})
            time.sleep(0.1)
            self._cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"button": "left", "down": False}}]}})
            time.sleep(0.1)
        time.sleep(0.4)

    def screenshot(self, name):
        path = os.path.join(self.workdir, name)
        self._cmd({"execute": "screendump", "arguments": {"filename": path}})
        time.sleep(0.8)
        return path

    def reset(self):
        """Warm-reset the guest. Disk contents survive, so this is how the
        persistence tests check that a file written before the reset is still
        there afterwards."""
        self._cmd({"execute": "system_reset"})
        self._read = 0
        self._buf = ""
        time.sleep(1.0)

    def power_cycle(self):
        """Yank the power: kill QEMU outright and start it again on the same
        disks. Models the unclean shutdown the journal has to survive."""
        if self.proc:
            self.proc.kill()
            self.proc.wait()
        try:
            self.qmp.close()
        except Exception:
            pass
        self._read = 0
        self._buf = ""
        self.port = _free_port()
        for i, v in enumerate(self.args):
            if v.startswith("tcp:127.0.0.1:"):
                self.args[i] = "tcp:127.0.0.1:%d,server,nowait" % self.port
        self.start()
