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
import json, os, re, shutil, socket, subprocess, time

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
                 boot_img=None, data_disks=None, no_reboot=False, nic="e1000",
                 hostfwd=None, audio="ac97", wav=None):
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

        # `wav` points QEMU's audio backend at a file instead of a null sink, so
        # a test can assert on the samples that actually left the guest rather
        # than on how long the guest thought it took to send them. The header's
        # length field is only fixed up when QEMU exits, so read it after stop().
        self.wav_path = None
        if wav:
            self.wav_path = wav if os.path.isabs(wav) else os.path.join(self.workdir, wav)
            # Pin the backend to the guest's own format, so what lands in the
            # file is the samples the codec emitted rather than a resampled
            # rendering of them (QEMU's wav backend defaults to 44.1 kHz).
            audiodev = ("wav,id=snd0,path=%s,out.frequency=48000,out.channels=2"
                        ",out.format=s16" % self.wav_path)
        else:
            audiodev = "none,id=snd0"

        a += ["-m", memory, "-smp", str(smp), "-cpu", "max", "-rtc", "base=utc",
              "-vga", "std", "-global", "VGA.vgamem_mb=64",
              "-audiodev", audiodev]
        # `audio` picks the emulated codec, so the same test body can be run
        # against either driver BoltOS ships. "hda" also attaches a codec: an
        # HDA controller with nothing on its link is a controller with nothing
        # to play through, and the driver is right to decline it.
        if audio in ("ac97", "both"):
            a += ["-device", "AC97,audiodev=snd0"]
        if audio in ("hda", "both"):
            a += ["-device", "intel-hda,id=hda",
                  "-device", "hda-duplex,bus=hda.0,audiodev=snd0"]
        elif audio == "hda-nocodec":
            a += ["-device", "intel-hda,id=hda"]
        if net:
            # `nic` picks the emulated adapter, so the same test body can be run
            # against every driver BoltOS ships: "e1000" or "virtio-net-pci".
            # `hostfwd` is a list of (host_port, guest_port) pairs, which is how
            # a test reaches a server running INSIDE the guest -- slirp gives the
            # guest no address the host can dial otherwise.
            nd = "user,id=net0"
            for hp, gp in (hostfwd or []):
                nd += ",hostfwd=tcp::%d-:%d" % (hp, gp)
            a += ["-netdev", nd, "-device", nic + ",netdev=net0"]
        a += ["-display", "none",
              "-serial", "file:" + self.serial_path,
              "-qmp", "tcp:127.0.0.1:%d,server,nowait" % self.port]
        # -no-reboot makes QEMU *exit* on any reset request, QMP system_reset
        # included, so it is off by default: reset() and the guest's own `reboot`
        # both need the machine to come back. Tests that want a reboot loop to
        # show up as an exit ask for it explicitly.
        if no_reboot:
            a += ["-no-reboot"]
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
        # QEMU's stderr goes to a file, not a pipe. A pipe nobody reads holds
        # 64 KiB and then blocks the writer, and a blocked QEMU is a guest that
        # has stopped dead -- no serial output, no input, no clue why. That is
        # not hypothetical: a chatty QEMU froze guests mid-test and the only
        # symptom was a machine that had printed its shell prompt and would
        # never answer again.
        self._errf = open(self.serial_path + ".stderr", "wb")
        self.proc = subprocess.Popen(self.args, stdout=subprocess.DEVNULL,
                                     stderr=self._errf)
        self._connect_qmp()

    def qemu_stderr(self):
        try:
            self._errf.flush()
            with open(self.serial_path + ".stderr", "rb") as fh:
                return fh.read().decode("utf-8", "replace")
        except (OSError, AttributeError):
            return ""

    def _connect_qmp(self):
        for _ in range(160):
            if self.proc.poll() is not None:
                raise SystemExit("QEMU exited early:\n" + self.qemu_stderr())
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

    def clear(self):
        """Forget everything seen so far. Output already written stays in the
        log file but stops matching, so a test can wait for the NEXT occurrence
        of a line that has appeared before -- e.g. the boot banner after a
        reboot, since a warm reset keeps appending to the same serial file."""
        self.serial()
        self._buf = ""

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

    def reach_shell(self, timeout=180, entry="2"):
        """Pick a boot-menu entry and wait until the shell is really reading.

        Not expect("BoltShell ready"). /bin/hello runs in ring 3 on another
        thread and writes to the same serial port, so the banner routinely
        arrives cut in half -- "BoltShell rea" ... "dy. 134 commands" -- and no
        substring of it is safe to wait for. Twenty-two tests each carried a
        copy of that wait and each could lose the race; t_net's author hit it,
        documented it, and fixed it in one file.

        Probing is reliable where matching a banner is not: if a marker typed
        at the prompt comes back, the shell is reading input, which is the
        thing every caller actually wants to know.

        `entry` is the boot-menu key -- "2" for safe mode (a text shell, no
        desktop or network), "1" for a normal boot. Returns the boot log,
        which probing would otherwise clear and which callers assert on.
        """
        # Keep offering the menu key until the kernel says which mode it chose.
        # Not a fixed number of presses: if a slow host outlasted them we would
        # start probing while stage2 was still counting down, and the probe's
        # Enter would pick the *default* entry -- a normal boot with a desktop,
        # where a test expecting a text shell then waits for a prompt that is
        # never coming.
        deadline = time.time() + timeout
        while time.time() < deadline and "boot mode" not in self.serial():
            self.key(entry)
            time.sleep(0.4)

        # Let the boot output stop before typing anything. Silence is the one
        # signal about boot progress that cannot arrive cut in half, and typing
        # into a machine that is still bringing its drivers up is both pointless
        # and the worst case for the PS/2 controller. Bounded, so a machine that
        # never goes quiet still gets probed.
        quiet_since = time.time()
        last = len(self.serial())
        while time.time() < deadline:
            time.sleep(0.3)
            now = len(self.serial())
            if now != last:
                last = now
                quiet_since = time.time()
            elif time.time() - quiet_since > 1.5:
                break

        # The marker is deliberately all lowercase letters. Every other
        # character on this keyboard needs Shift, and a probe that depends on a
        # modifier is a probe that a stray modifier state can defeat -- which
        # is a silly way to spend a three-minute timeout.
        boot_log = ""
        while time.time() < deadline:
            boot_log += self.serial()
            self._buf = ""                     # probe against a clean buffer
            self.key("ret")                    # flush any half-typed line
            # The leading space is the shell's own opt-out from history.
            # Without it the probe becomes the most recent thing the user
            # typed, and a test that presses Up to check what it typed before a
            # reboot finds this instead of its own command.
            self.type(" echo boltvmshellup\n")
            time.sleep(1.2)
            if "boltvmshellup" in self.serial():
                time.sleep(0.4)
                # Put the boot log back. Probing consumed it, and it is the only
                # place the driver bind, the DHCP lease and the filesystem mount
                # are reported -- callers assert on those straight afterwards,
                # so losing them here would break them in a way that looks like
                # the kernel stopped printing.
                self._buf = boot_log + self.serial()
                return self._buf
        raise AssertionError("never reached a shell prompt\n--- serial tail ---\n"
                             + self.serial()[-2500:])

    def wait_prompt(self, timeout=60):
        """Block until the shell prints its prompt.

        "BoltShell ready" is printed before the shell reaches its read loop, so
        typing a fixed moment after it is a guess -- and on a loaded host the
        guess is sometimes wrong, which is how a test fails only when the
        machine is busy. Waiting for the prompt is the same thing run() does
        after a command, applied to the first one.
        """
        end = time.time() + timeout
        reprompted = False
        while time.time() < end:
            # The boot-time ring-3 demo can print between the prompt's `>`
            # and its space. The delimiter itself proves the shell is ready.
            if re.search(r"(?:^|\n)bolt:[^\r\n>]*>", self.serial()):
                return True
            # It may also interrupt the middle of "bolt:". After that demo
            # exits, submit one empty line to get an uninterrupted prompt.
            # Do not type the actual test command until that prompt arrives.
            if (not reprompted and "BoltShell ready." in self.serial() and
                    "[exit] pid=1 code=0" in self.serial()):
                self.key("ret")
                reprompted = True
            time.sleep(0.25)
        raise AssertionError("shell prompt did not arrive:\n" + self.serial()[-3000:])

    # ---- input -----------------------------------------------------------
    def _keyev(self, qcode, down):
        return {"type": "key", "data": {"down": down,
                                        "key": {"type": "qcode", "data": qcode}}}

    def key(self, qcode, shift=False, ctrl=False, alt=False, meta=False):
        """Press and release one key, optionally with modifiers held.

        `ctrl` is what lets a test drive anything that reads control keys --
        the editor's ^S / ^X, a shell interrupt -- which cannot be expressed as
        a character through type(). `meta` is the Super/Win key, which the
        compositor reserves for itself: window snapping, the Start menu and
        the virtual desktops."""
        ev = []
        if shift: ev.append(self._keyev("shift", True))
        if ctrl:  ev.append(self._keyev("ctrl", True))
        if alt:   ev.append(self._keyev("alt", True))
        if meta:  ev.append(self._keyev("meta_l", True))
        ev += [self._keyev(qcode, True), self._keyev(qcode, False)]
        if meta:  ev.append(self._keyev("meta_l", False))
        if alt:   ev.append(self._keyev("alt", False))
        if ctrl:  ev.append(self._keyev("ctrl", False))
        if shift: ev.append(self._keyev("shift", False))
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

    def run(self, line, timeout=90):
        """Type a shell command and block until the prompt comes back.

        Waiting for the prompt instead of sleeping a fixed time is what keeps
        the suite honest on a busy machine: every fixed sleep is a guess that
        stops being true the moment the host is loaded, and a test that fails
        only under load is worse than no test at all.
        """
        self.clear()
        self.type(line + "\n")
        end = time.time() + timeout
        while time.time() < end:
            tail = self.serial().rstrip()
            if tail.endswith("bolt:/>") or tail.endswith(">"):
                return self.serial()
            time.sleep(0.25)
        # No prompt came back. Returning what we have keeps callers that expect
        # a command not to return (reboot, poweroff) working, but a silent empty
        # string turns "the guest never answered" into "the output was wrong",
        # and those two are investigated in completely different places.
        return self.serial() + (
            "\n[boltvm: no prompt %ds after %r -- the guest did not respond]\n"
            % (timeout, line))

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
        there afterwards. Needs no_reboot=False (the default)."""
        self.clear()
        self._cmd({"execute": "system_reset"})
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
