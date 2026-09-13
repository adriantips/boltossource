#!/usr/bin/env python3
"""Keyboard layouts: what the key you press actually produces.

Two halves, for two different kinds of bug.

**The tables, on the host.** A layout is data, and data with one entry in the
wrong place is the most quietly annoying thing an OS can ship: everything
works, and then one key on one layout does the wrong thing, and the person it
happens to assumes they are holding it wrong. test/host/kbdlayout.c compiles
kernel/kbdlayout.c with stubs and checks every letter of every layout against
what is printed on a real key cap, in a millisecond, with no VM. That is the
only way this gets checked properly -- typing at a QEMU window can cover a
handful of keys on one layout in about a minute.

That half has already earned itself. The digit row starts at scancode 0x02, so
the key marked '3' is 0x04, and half the layouts had their digit-row overrides
one place out: a keyboard wrong in exactly one spot each.

**The driver, in QEMU.** The table being right does not mean the right entry is
reached. Three things only the real path can show:

  * AltGr is not Alt. The right Alt key used to be folded into Alt, so AltGr+Q
    fired whatever Alt+Q was bound to and typed nothing -- which on a German
    keyboard means there is no way at all to type an at-sign. QEMU can send a
    real `altgr` keypress, so this types one.
  * A dead key types nothing, and then changes the next letter. Pressing the
    acute on a German layout has to produce no character at all, and then the
    'e' after it has to arrive as one byte, not two.
  * The layout survives a reboot, because /etc/keyboard.conf is the only
    reason this is not a variable.

Everything is Latin-1. The test says so rather than quietly limiting itself.
"""
import os, re, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

LOGIN_USER, LOGIN_PASS = "user", "bolt"
OUT = Path(ROOT) / "test" / "out"


def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")

    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("kbd-host.exe" if os.name == "nt" else "kbd-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Wno-inconsistent-dllimport",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "kbdlayout.c"),
                    str(Path(ROOT) / "kernel" / "kbdlayout.c"),
                    "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(r.stdout.rstrip())
    if r.stderr.strip():
        print(r.stderr.rstrip())
    assert r.returncode == 0, "kernel/kbdlayout.c disagrees with the key caps"
    print("  ok: every key of every layout matches what is printed on it")


def run(vm, line, wait_for, timeout=60):
    vm.clear()
    vm.type(line + "\n")
    assert vm.wait_for(wait_for, timeout=timeout), \
        "%r never printed %r:\n%s" % (line, wait_for, vm.serial()[-1000:])
    return vm.serial()


def typed(vm, keys, timeout=30):
    """Send raw key events, then echo what the shell received, as RAW BYTES.

    `echo` is the read-back: whatever bytes the driver put in the ring come out
    of the serial port, which is the only way to see a character the host
    cannot easily send as text.

    Raw bytes, and not vm.serial(), for a reason that cost an hour: vm.serial()
    decodes the log as UTF-8 with replacement, and a lone 0xE9 -- which is
    exactly what an e-acute is in Latin-1 -- is not valid UTF-8, so it arrives
    here as a replacement character. The guest was right the whole time and the
    test could not see it. Everything this file asserts about typed characters
    is therefore a byte comparison.
    """
    mark = b"echo "
    vm.clear()
    before = os.path.getsize(vm.serial_path)
    vm.type("echo ")
    for qcode, mods in keys:
        vm.key(qcode, **mods)
        time.sleep(0.08)
    vm.key("ret")
    assert vm.wait_for("\n", timeout=timeout), "the shell never answered"
    time.sleep(0.6)
    with open(vm.serial_path, "rb") as fh:
        fh.seek(before)
        raw = fh.read()
    # What the shell echoed back, after the last "echo " it saw.
    i = raw.rfind(mark)
    tail = raw[i + len(mark):] if i >= 0 else raw
    # The command line itself, then its output. Both carry the bytes; taking
    # the last non-empty line avoids depending on which.
    lines = [l.strip() for l in tail.replace(b"\r", b"").split(b"\n") if l.strip()]
    return lines[-1] if lines else b""


# Which physical key to press, on a German layout, to get each character of a
# Unix path. Only two differ from US, and both of them matter: the slash is
# Shift+7 (there is no unshifted slash on a German keyboard at all) and 'y' is
# on the key marked Z.
DE_KEYS = {"/": ("7", {"shift": True}), "y": ("z", {}), ".": ("dot", {}),
           "_": ("slash", {"shift": True}), " ": ("spc", {})}


def de_type(vm, text):
    """Type `text` as a person sitting at a German keyboard would.

    This exists because the obvious version of the persistence check typed
    `cat /etc/keyboard.conf` with US key events while the guest was on German,
    and the guest dutifully read it as `cat -etc-kezboard.conf`. That is the
    feature working; the test was the thing that was wrong.
    """
    for ch in text:
        if ch in DE_KEYS:
            q, mods = DE_KEYS[ch]
            vm.key(q, **mods)
        elif ch.isalnum():
            vm.key(ch)
        else:
            raise AssertionError("de_type has no key for %r" % ch)
        time.sleep(0.04)
    vm.key("ret")


def guest_tests():
    with BoltVM(name="kbd", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=200)
        log = vm.serial()
        vm.open_app("Terminal")

        assert "keyboard layout: us" in log, \
            "the boot log should name the layout:\n" + log[-900:]
        print("  ok: boots on us, and says so")

        out = run(vm, "kbd", "US English")
        for name in ("us-intl", "uk", "de", "fr", "es", "it", "se"):
            assert re.search(r"\b" + re.escape(name) + r"\b", out), \
                "`kbd` does not offer %s:\n%s" % (name, out[-800:])
        assert "Latin-1 only" in out, \
            "the layout list should say what it cannot do:\n" + out[-600:]
        print("  ok: eight layouts listed, with the Latin-1 limit stated")

        out = run(vm, "kbd show de", "layout:")
        assert "de" in out and "altgr" in out, out[-700:]
        # Looking must not switch.
        out2 = run(vm, "kbd", "US English")
        assert re.search(r"\* us\b", out2), \
            "`kbd show de` left the machine on German:\n" + out2[-600:]
        print("  ok: `kbd show NAME` looks without switching")

        # ---- German, for real ------------------------------------------
        run(vm, "kbd de", "German")

        # QWERTZ: the physical Y key must now produce z. QEMU sends key events
        # by name of the US key cap, which is exactly what a real keyboard's
        # scancode is, so this goes through the same path a person would.
        got = typed(vm, [("y", {}), ("z", {})])
        assert got == b"zy", \
            "QWERTZ: the Y and Z keys should type 'zy', got %r" % got
        print("  ok: the Y and Z keys swap on German")

        # AltGr+Q is an at-sign, and is the ONLY way to type one here.
        got = typed(vm, [("q", {"altgr": True})])
        assert got == b"@", \
            ("AltGr+Q should be an at-sign -- on a German keyboard there is no "
             "other way to type one at all. Got %r" % got)
        print("  ok: AltGr+Q is an at-sign -- AltGr is not Alt")

        # A dead key: the acute (the key right of sharp-s, US '=') types
        # nothing, and then 'e' arrives as ONE byte, e-acute. Two bytes would
        # mean the accent was emitted and the letter typed after it.
        got = typed(vm, [("equal", {}), ("e", {})])
        assert got == b"\xe9", \
            ("a dead acute then 'e' should be one byte 0xE9 (e-acute); got %r. "
             "Two bytes means the accent was not pending, one wrong byte means "
             "the compose table is." % got)
        print("  ok: a dead key composes with the letter after it")

        # The accent on its own, which is how you type one.
        got = typed(vm, [("equal", {}), ("spc", {})])
        assert got == b"\xb4", \
            "a dead acute then space should be the bare accent 0xB4; got %r" % got
        print("  ok: a dead key then space gives the accent itself")

        # A pair with no accented form. Both characters, in order -- which is
        # what every other system does, and needs the compose to say "no"
        # rather than guess.
        # 's', not 'y'. The first version of this used the key marked Z, which
        # types 'y' on QWERTZ -- and Latin-1 *has* a y-acute, so the assertion
        # was the opposite of what it meant to say and the code was right.
        got = typed(vm, [("equal", {}), ("s", {})])
        assert got == b"\xb4s", \
            ("acute + s has no combined form in Latin-1, so both should come "
             "out in order: expected b'\\xb4s', got %r" % got)
        print("  ok: a pair that does not combine emits both, in order")

        # And the umlaut keys, which are the reason anyone picks this layout.
        got = typed(vm, [("apostrophe", {}), ("semicolon", {}), ("bracket_left", {})])
        assert got == b"\xe4\xf6\xfc", \
            "the three umlaut keys should type a-o-u dieresis; got %r" % got
        print("  ok: the umlaut keys type umlauts")

        # ---- persistence ------------------------------------------------
        # Typed on the German layout, which is both how the file gets read and
        # a last check that a whole path comes out right rather than one key.
        vm.clear()
        de_type(vm, "cat /etc/keyboard.conf")
        assert vm.wait_for("layout", timeout=30), \
            "reading the config on a German keyboard failed:\n" + vm.serial()[-600:]
        out = vm.serial()
        assert "layout = de" in out, out[-500:]
        print("  ok: a whole Unix path types correctly on German")
        vm.reset()
        vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=200)
        log = vm.serial()
        assert "keyboard layout: de" in log, \
            "the layout did not survive a reboot:\n" + log[-900:]
        print("  ok: the layout is still German after a reboot")

        vm.open_app("Terminal")
        run(vm, "kbd us", "US English")
        got = typed(vm, [("y", {})])
        assert got == b"y", "switching back to US did not take; got %r" % got
        print("  ok: switching back to US takes effect immediately")

        vm.expect_absent("KERNEL PANIC")


def main():
    host_tests()
    if "--host-only" in sys.argv:
        print("PASS kbd: the layout tables only (--host-only)")
        return 0
    guest_tests()
    print("PASS kbd: eight layouts, a real AltGr, and dead keys that compose")
    return 0


if __name__ == "__main__":
    sys.exit(main())
