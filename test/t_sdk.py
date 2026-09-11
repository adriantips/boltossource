#!/usr/bin/env python3
"""The SDK, end to end: a program the kernel has never heard of.

BoltOS ran ring-3 programs before this, but every one of them was started by a
command the kernel was built with -- `js` ran /bin/js, `webx` ran /bin/browser.
There was no way to run a program that did not ship with the OS, and no
document saying what such a program could rely on. Both are the same problem.

This test takes the whole path SDK.md describes:

    sdk/build.sh          compile and link outside BoltOS, with the host
                          toolchain and the published headers
    HTTP                  carry it onto a running machine
    download / run        put it in /bin and start it, with arguments

and then checks the things the ABI actually promises: the version the kernel
reports, argv arriving intact, the filesystem syscalls working, floating point
working, and the exit status coming back to the shell.
"""
import os, shutil, struct, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

OUT = os.path.join(ROOT, "test", "out")
APP = os.path.join(OUT, "sdkhello")
GUEST_HOST = "10.0.2.2"          # the host, as QEMU user networking presents it


def build_app():
    os.makedirs(OUT, exist_ok=True)
    bash = shutil.which("bash") or r"C:\msys64\usr\bin\bash.exe"
    env = dict(os.environ)
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\usr\bin;C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    # Two source files with the same basename must produce distinct objects.
    for directory, symbol, value in (("one", "one", 1), ("two", "two", 2)):
        d = os.path.join(OUT, directory)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "value.c"), "w") as f:
            f.write("int %s(void) { return %d; }\n" % (symbol, value))
    r = subprocess.run([bash, os.path.join(ROOT, "sdk", "build.sh"), "-o", APP,
                        os.path.join(ROOT, "sdk", "hello.c"), "one/value.c", "two/value.c"],
                       cwd=OUT, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError("SDK build failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    with open(APP, "rb") as fh:
        magic = fh.read(4)
    assert magic == b"\x7fELF", "sdk/build.sh did not produce an ELF"
    print("  ok: sdk/build.sh linked a BoltOS ELF with the published headers")
    return True


def serve(directory):
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=directory, **kw)

        def log_message(self, *a):
            pass

    srv = ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def reach_net_shell(vm, timeout=180):
    """Normal boot with no framebuffer: a serial shell with the network up."""
    return vm.reach_shell(timeout, "1")


def main():
    if not build_app():
        return 0

    srv, port = serve(OUT)
    try:
        with BoltVM(name="sdk", extra=["-vga", "none"]) as vm:
            reach_net_shell(vm)

            out = vm.run("download http://%s:%d/sdkhello /bin/sdkhello" % (GUEST_HOST, port),
                         timeout=120)
            assert "downloaded" in out, "guest could not download SDK program:\n" + out
            vm.run("chmod 755 /bin/sdkhello")
            print("  ok: the program was carried onto the machine over HTTP")

            out = vm.run("run sdkhello", timeout=120)
            assert "sdkhello: BoltOS ABI 1" in out, \
                "the program did not run, or the ABI is not what it says:\n" + out
            assert "argv[0] = /bin/sdkhello" in out, "argv[0] is wrong:\n" + out
            assert "read back" in out and "kernel never heard of" in out, \
                "the filesystem syscalls did not work:\n" + out
            assert "sqrt(2) = 1.41" in out, "floating point did not survive:\n" + out
            assert "run: /bin/sdkhello exited 0" in out, \
                "the exit status did not come back to the shell:\n" + out
            print("  ok: `run` started it, argv and the syscalls worked")

            # Arguments through, and a non-zero status reported as itself.
            out = vm.run('run sdkhello 7 "two words" ; echo result=$?', timeout=120)
            assert "argv[1] = 7" in out, "the argument did not arrive:\n" + out
            assert "run: /bin/sdkhello exited 7" in out, \
                "a non-zero exit status was not reported:\n" + out
            assert "argv[2] = two words" in out and "result=7" in out, out
            print("  ok: arguments arrive and the exit status is reported")

            # And what it wrote is really on the filesystem.
            out = vm.run("cat /tmp/sdkhello.txt")
            assert "kernel never heard of" in out, \
                "the file the program wrote is not there:\n" + out
            print("  ok: what it wrote is on the filesystem afterwards")

            # A program that is not an ELF is refused rather than loaded.
            vm.run("write /tmp/notaprogram hello")
            out = vm.run("run /tmp/notaprogram")
            assert "not an ELF" in out, "a non-program was not refused:\n" + out
            print("  ok: `run` refuses something that is not a program")

            # Malformed external binaries must be rejected before they map pages.
            with open(APP, "rb") as f:
                original = f.read()
            phoff = struct.unpack_from("<Q", original, 32)[0]
            variants = {
                "bad-phoff": (32, "<Q", 0xfffffffffffffff0),
                "bad-phsize": (54, "<H", 1),
                "kernel-segment": (phoff + 16, "<Q", 0xffff800000100000),
                "oversized-file": (phoff + 32, "<Q", len(original) + 4096),
                "bad-entry": (24, "<Q", 0xffff800000100000),
            }
            for name, (offset, fmt, value) in variants.items():
                bad = bytearray(original)
                struct.pack_into(fmt, bad, offset, value)
                with open(os.path.join(OUT, name), "wb") as f:
                    f.write(bad)
                out = vm.run("download http://%s:%d/%s /tmp/%s" % (GUEST_HOST, port, name, name))
                assert "downloaded" in out, out
                vm.run("chmod 755 /tmp/" + name)
                out = vm.run("run /tmp/" + name)
                assert "bad ELF" in out and "could not start" in out, out
            out = vm.run("run sdkhello", timeout=120)
            assert "run: /bin/sdkhello exited 0" in out, out
            out = vm.run("run crashme null ; echo fault=$?", timeout=120)
            assert "exited 142" in out and "fault=142" in out, out
            print("  ok: malformed ELFs are refused and a ring-3 fault returns its status")

            vm.run("cp /bin/sdkhello /tmp/noexec")
            vm.run("chmod 644 /tmp/noexec")
            vm.run("users su user bolt")
            out = vm.run("run /tmp/noexec ; echo denied=$?")
            assert "permission denied" in out and "denied=126" in out, out
            print("  ok: a signed-in user cannot execute a file without execute permission")

            vm.expect_absent("KERNEL PANIC")
    finally:
        srv.shutdown()

    print("PASS sdk: a program built outside BoltOS runs on BoltOS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
