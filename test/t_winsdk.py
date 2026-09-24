#!/usr/bin/env python3
"""The Windows SDK, end to end: a project that did not exist, running on BoltOS.

t_sdk.py already proves the ABI -- that an ELF built with the published headers
loads and runs. This is about everything around it: sdk/bolt.ps1 makes a
project, builds it, packages it, and serves a repository from the Windows box
it was written on, and BoltOS installs from that repository like any other.

The host half runs without QEMU, because the things most likely to break here
break on the host: a linker script that stops matching the loader, a package
whose framing is off by one byte, an index hash computed in the wrong integer
width. Those are checked against the format as kernel/pkg.c reads it, by code
that shares nothing with the PowerShell that produced them.

The guest half needs iso/os.img and prints SKIP without it.
"""
import os, shutil, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT, _free_port

OUT = os.path.join(ROOT, "test", "out", "winsdk")
BOLT = os.path.join(ROOT, "sdk", "bolt.cmd")
APP = "sdkwin"
GUEST_HOST = "10.0.2.2"          # the host, as QEMU user networking presents it


def fnv1a(data):
    """kernel/pkg.c's hash_hex, in Python. Two implementations that must agree
    is the point -- a third one in PowerShell is what this is checking."""
    h = 0xcbf29ce484222325
    for c in data:
        h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def bolt(*args, **kw):
    r = subprocess.run([BOLT] + list(args), capture_output=True, text=True,
                       cwd=kw.get("cwd", ROOT))
    if r.returncode != 0 and not kw.get("allow_fail"):
        raise AssertionError("bolt %s failed (%d):\n%s%s"
                             % (" ".join(args), r.returncode, r.stdout, r.stderr))
    return r


def host_half():
    """new -> build -> pack -> stage, then read the results back as BoltOS would."""
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)

    bolt("new", APP, "-dir", OUT)
    proj = os.path.join(OUT, APP)
    for f in ("pkg.txt", "main.c"):
        assert os.path.exists(os.path.join(proj, f)), "bolt new left out " + f
    print("  ok: `bolt new` wrote a project with a manifest and a program")

    bolt("build", proj)
    binary = os.path.join(proj, "build", APP)
    blob = open(binary, "rb").read()

    # The loader's rules, from SDK.md section 1. A program that fails any of
    # these is refused before a page is mapped, which is a miserable thing to
    # discover at the far end of a download.
    assert blob[:4] == b"\x7fELF" and blob[4] == 2 and blob[5] == 1, "not an ELF64 LSB file"
    etype, machine = struct.unpack_from("<HH", blob, 16)
    assert (etype, machine) == (2, 62), "expected a static ET_EXEC for x86-64, got %d/%d" % (etype, machine)
    entry, phoff = struct.unpack_from("<QQ", blob, 24)
    phentsize, phnum = struct.unpack_from("<HH", blob, 54)
    loadable = []
    for i in range(phnum):
        o = phoff + i * phentsize
        ptype, flags = struct.unpack_from("<II", blob, o)
        vaddr = struct.unpack_from("<Q", blob, o + 16)[0]
        memsz = struct.unpack_from("<Q", blob, o + 40)[0]
        if ptype == 1:
            loadable.append((vaddr, memsz, flags))
    assert loadable, "no PT_LOAD segments"
    for vaddr, memsz, _ in loadable:
        assert vaddr >= 0x1000, "a segment below 0x1000 will be refused"
        assert vaddr + memsz < 0x0000400000000000, "a segment runs into the mmap region"
    assert any(f & 1 and v <= entry < v + m for v, m, f in loadable), \
        "the entry point is not inside an executable segment"
    assert any(v == 0x400000 for v, _, _ in loadable), "not loaded at 0x400000"
    print("  ok: `bolt build` linked an ELF the loader will accept (%d bytes)" % len(blob))

    bolt("pack", proj)
    pkg = os.path.join(proj, "build", "%s-1.0.pkg" % APP)
    img = open(pkg, "rb").read()

    # Parse it the way kernel/pkg.c's unpack() does: header, blank line, then
    # `file SIZE PATH` and exactly that many raw bytes.
    head, sep, body = img.partition(b"\n\n")
    assert sep, "no blank line ends the package header"
    fields = dict(l.split(" ", 1) for l in head.decode().splitlines() if " " in l)
    assert head.startswith(b"BOLTPKG 1\n"), "wrong magic line"
    assert fields["package"] == APP and fields["version"] == "1.0", fields
    assert fields["kind"] == "program", fields

    p, carried = 0, {}
    while p < len(body):
        nl = body.index(b"\n", p)
        line = body[p:nl].decode()
        p = nl + 1
        assert line.startswith("file "), "unexpected body line %r" % line
        size, path = line[5:].split(" ", 1)
        carried[path] = body[p:p + int(size)]
        p += int(size) + 1
    assert carried["/bin/%s" % APP] == blob, \
        "the ELF did not survive packaging byte for byte"
    print("  ok: `bolt pack` produced a package BoltOS's own parser can read")

    bolt("ship", proj, "-stage")
    ship = os.path.join(proj, "build", "ship")
    index = open(os.path.join(ship, "index"), "rb").read().decode()
    declared = [l.split(" ", 1)[1] for l in index.splitlines() if l.startswith("sha ")]
    assert declared, "the index has no sha line"
    assert declared[0] == fnv1a(img), \
        "index hash %s is not the package's (%s) -- install will refuse it" % (declared[0], fnv1a(img))
    assert "file %s-1.0.pkg" % APP in index, "the index does not name the package file:\n" + index
    assert os.path.exists(os.path.join(ship, APP)), "the raw binary was not staged for `download`"
    print("  ok: `bolt ship -stage` wrote a repository whose index hash checks out")
    return proj


def guest_half(proj):
    """Serve it with bolt's own server, and install it on a real BoltOS."""
    port = _free_port()
    srv = subprocess.Popen([BOLT, "ship", proj, "-port", str(port)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, cwd=ROOT)
    try:
        # Wait for it to say it is listening rather than racing the guest.
        deadline, listening = time.time() + 90, False
        while time.time() < deadline:
            line = srv.stdout.readline()
            if not line:
                break                      # it exited; the assert below says so
            if "serving" in line:
                listening = True
                break
        assert listening, "bolt ship never started serving (exit %s)" % srv.poll()

        url = "http://%s:%d" % (GUEST_HOST, port)
        with BoltVM(name="winsdk", extra=["-vga", "none"]) as vm:
            vm.reach_shell(180, "1")          # normal boot, no panel: shell + network

            out = vm.run("pkg repo " + url, timeout=60)
            assert url in out, "the repository was not set:\n" + out

            # The guest's first outbound connection after boot does not always
            # land -- it fails, and the next one works. Retrying here rather
            # than skipping: a transient miss used to look exactly like "this
            # machine has no route to the host", which quietly threw away the
            # half of this test that is worth having.
            for attempt in range(4):
                out = vm.run("pkg update", timeout=120)
                if "cannot read the index" not in out:
                    break
                print("    (pkg update attempt %d did not connect, retrying)" % (attempt + 1))
                time.sleep(4)
            else:
                print("  SKIP guest half: no route to the host's server after 4 tries")
                return False
            assert "1 package available" in out, \
                "the guest could not read the Windows repository's index:\n" + out
            print("  ok: BoltOS read an index served by sdk/bolt.ps1")

            out = vm.run("pkg install " + APP, timeout=180)
            assert "does not match its index entry" not in out, \
                "the hash the SDK wrote is not the one the kernel computes:\n" + out
            assert "installed" in out, "the package did not install:\n" + out
            print("  ok: it installed, hash checked, from a repository on Windows")

            vm.run("chmod 755 /bin/%s" % APP)
            out = vm.run("run %s hello" % APP, timeout=120)
            assert "%s: running as pid" % APP in out, \
                "the program the SDK built did not run:\n" + out
            assert "argv[1] = hello" in out, "arguments did not arrive:\n" + out
            assert "run: /bin/%s exited 0" % APP in out, \
                "the exit status did not come back:\n" + out
            print("  ok: `run` started it and the exit status came back")

            # The other road in: the raw binary, no package manager at all.
            out = vm.run("download %s/%s /tmp/%s" % (url, APP, APP), timeout=120)
            assert "downloaded" in out, "the raw binary did not come over:\n" + out
            vm.run("chmod 755 /tmp/" + APP)
            out = vm.run("run /tmp/%s" % APP, timeout=120)
            assert "exited 0" in out, "the downloaded binary did not run:\n" + out
            print("  ok: `download` of the same server's raw ELF works too")

            vm.expect_absent("KERNEL PANIC")
            return True
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=10)
        except subprocess.TimeoutExpired:
            srv.kill()


def main():
    if os.name != "nt":
        print("SKIP winsdk: the Windows SDK needs Windows")
        return 0
    if not os.path.exists(BOLT):
        print("SKIP winsdk: sdk/bolt.cmd is not here")
        return 0
    r = bolt("doctor", allow_fail=True)
    if r.returncode != 0:
        print("SKIP winsdk: no cross toolchain on this machine\n" + r.stdout)
        return 0

    proj = host_half()

    if not os.path.exists(os.path.join(ROOT, "iso", "os.img")):
        print("SKIP winsdk guest half: iso/os.img missing -- run build.sh first")
        print("PASS winsdk (host half only)")
        return 0

    if not guest_half(proj):
        print("PASS winsdk (host half only)")
        return 0
    print("PASS winsdk: a project made on Windows installs and runs on BoltOS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
