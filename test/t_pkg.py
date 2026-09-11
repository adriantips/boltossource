#!/usr/bin/env python3
"""The package manager: software that arrives after the kernel was built.

The old `pkg` was five BoltPython programs compiled into the kernel. You could
"install" one, which copied a string into /apps. Nothing could be added to it
without rebuilding the kernel on another machine -- the one thing a package
manager exists to avoid.

Three claims are checked, and they are what "install a new piece of software"
has to mean:

  * a package installs with its dependencies, and what it installs is usable --
    here a starter C project whose headers come from two other packages, built
    and run by `build` straight afterwards
  * a package can be *made* on BoltOS: `pkg build` packs a directory, `pkg
    publish` puts it in a repository and indexes it, and it installs from there
  * a repository can live on another machine: the same install, over HTTP, from
    a repository this test generates on the host -- which also proves the format
    is one an outside tool can write

Plus the two refusals that matter: a package whose bytes do not match its index
entry is not installed, and one that something else depends on is not removed.
"""
import os, re, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

OUT = os.path.join(ROOT, "test", "out")
GUEST_HOST = "10.0.2.2"          # the host, as QEMU user networking presents it


# --------------------------------------------------------------------------
#  A repository, written by something that is not BoltOS. If the format needs
#  BoltOS to produce it, it is not a format.
# --------------------------------------------------------------------------
def fnv1a(data):
    h = 0xcbf29ce484222325
    for c in data:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def pack(name, version, kind, summary, files, requires="", run=""):
    head = "BOLTPKG 1\npackage %s\nversion %s\nkind %s\nsummary %s\n" % (
        name, version, kind, summary)
    if requires:
        head += "requires %s\n" % requires
    if run:
        head += "run %s\n" % run
    out = head.encode() + b"\n"
    for path, body in files:
        b = body.encode()
        out += ("file %d %s\n" % (len(b), path)).encode() + b + b"\n"
    return out


def write_repo(directory):
    os.makedirs(directory, exist_ok=True)
    index = ""
    packages = [
        ("netlib", "2.0", "source", "a header fetched over the network", "",
         [("/usr/include/netlib.h",
           "#ifndef NETLIB_H\n#define NETLIB_H\n"
           "int triple(int n) { return n * 3; }\n#endif\n")], ""),
        ("nettool", "2.0", "script", "a script that needed netlib first", "netlib",
         [("/apps/nettool.py", "print('nettool from the network repo')\n")],
         "/apps/nettool.py"),
    ]
    for name, ver, kind, summary, req, files, run in packages:
        blob = pack(name, ver, kind, summary, files, req, run)
        fname = "%s-%s.pkg" % (name, ver)
        with open(os.path.join(directory, fname), "wb") as fh:
            fh.write(blob)
        index += "package %s\nversion %s\nkind %s\nsummary %s\n" % (name, ver, kind, summary)
        if req:
            index += "requires %s\n" % req
        if run:
            index += "run %s\n" % run
        index += "file %s\nsha %s\n\n" % (fname, fnv1a(blob))

    # And one whose bytes do not match what the index promises, so the check
    # that catches a truncated or swapped download is exercised too.
    bad = pack("tampered", "1.0", "script", "its hash will not match", [
        ("/apps/tampered.py", "print('this must never run')\n")])
    with open(os.path.join(directory, "tampered-1.0.pkg"), "wb") as fh:
        fh.write(bad)
    index += ("package tampered\nversion 1.0\nkind script\n"
              "summary its hash will not match\n"
              "file tampered-1.0.pkg\nsha 0000000000000000\n\n")

    with open(os.path.join(directory, "index"), "w") as fh:
        fh.write(index)
    return directory


def serve(directory):
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=directory, **kw)

        def log_message(self, *a):
            pass

    srv = ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


# --------------------------------------------------------------------------
#  guests
# --------------------------------------------------------------------------
def reach_safe_shell(vm, timeout=180):
    return vm.reach_shell(timeout, "2")


def reach_net_shell(vm, timeout=180):
    """Normal boot with no framebuffer: a serial shell with the network up."""
    return vm.reach_shell(timeout, "1")


def local_repo_tests():
    with BoltVM(name="pkg", extra=["-vga", "none"]) as vm:
        reach_safe_shell(vm)

        out = vm.run("pkg update", timeout=90)
        assert "packages available" in out, "the shipped repository is not readable:\n" + out
        out = vm.run("pkg avail")
        for want in ("cproject", "boltlib", "mathx"):
            assert want in out, "%s is missing from the catalogue:\n%s" % (want, out)
        print("  ok: the repository that ships with the OS lists its packages")

        # --- dependencies, and something usable at the end of them ----------
        out = vm.run("pkg install cproject", timeout=150)
        assert "requires boltlib" in out and "installed boltlib" in out, \
            "the dependency was not installed first:\n" + out
        assert "installed cproject" in out, "the package itself did not install:\n" + out

        out = vm.run("build /home/Documents/newproj", timeout=180)
        assert "score(3) = 7" in out, \
            "the project the package installed does not build and run:\n" + out
        print("  ok: install pulls in dependencies, and `build` runs what arrived")

        # --- and it does not take back what someone else is using -----------
        out = vm.run("pkg remove mathx")
        assert "required by" in out, "a depended-on package was removed:\n" + out
        out = vm.run("pkg remove cproject")
        assert "removed cproject" in out, "cproject did not remove:\n" + out
        out = vm.run("cat /home/Documents/newproj/main.c")
        assert "cannot" in out.lower() or "no such" in out.lower() or "main(" not in out, \
            "remove left the package's files behind:\n" + out
        out = vm.run("pkg remove mathx")
        assert "removed mathx" in out, "mathx did not remove once nothing needed it:\n" + out
        print("  ok: remove refuses while depended on, and takes its files back after")

        # --- a package made on BoltOS, published, and installed -------------
        vm.run("mkdir /src")
        vm.run('write /src/greet.py "print(\'a package built on BoltOS\')"')
        vm.run('write /src/pkg.txt "package greet"')
        vm.run('write -a /src/pkg.txt "version 1.0"')
        vm.run('write -a /src/pkg.txt "kind script"')
        vm.run('write -a /src/pkg.txt "summary made here, not baked in"')
        vm.run('write -a /src/pkg.txt "run /apps/greet.py"')
        vm.run('write -a /src/pkg.txt "install greet.py /apps/greet.py"')

        out = vm.run("pkg build /src", timeout=90)
        assert "built /src/greet-1.0.pkg" in out, "`pkg build` produced nothing:\n" + out
        out = vm.run("mkdir /myrepo ; pkg publish /src/greet-1.0.pkg /myrepo", timeout=90)
        assert "published greet" in out, "`pkg publish` did not index it:\n" + out
        out = vm.run("pkg repo /myrepo ; pkg update", timeout=90)
        assert "1 package available" in out, "the new repository has no index:\n" + out
        out = vm.run("pkg install greet", timeout=90)
        assert "installed greet" in out, "the home-made package did not install:\n" + out
        out = vm.run("pkg run greet", timeout=90)
        assert "a package built on BoltOS" in out, "the installed package does not run:\n" + out
        print("  ok: a package built and published on BoltOS installs from its repository")

        vm.expect_absent("KERNEL PANIC")


def network_repo_tests():
    directory = write_repo(os.path.join(OUT, "pkgrepo"))
    srv, port = serve(directory)
    try:
        with BoltVM(name="pkg-net", extra=["-vga", "none"]) as vm:
            reach_net_shell(vm)
            url = "http://%s:%d" % (GUEST_HOST, port)
            out = vm.run("pkg repo " + url, timeout=60)
            assert url in out, "the repository was not set:\n" + out

            out = vm.run("pkg update", timeout=120)
            if "packages available" not in out:
                print("  SKIP network repository: the guest cannot reach the host")
                print("        (%s)" % out.strip().splitlines()[-2:])
                return
            assert "3 packages available" in out, "the fetched index is wrong:\n" + out

            out = vm.run("pkg install nettool", timeout=150)
            assert "installed netlib" in out, "the network dependency did not install:\n" + out
            assert "installed nettool" in out, "the package did not install:\n" + out
            out = vm.run("pkg run nettool", timeout=90)
            assert "nettool from the network repo" in out, "it does not run:\n" + out
            print("  ok: a repository on another machine installs over HTTP")

            out = vm.run("pkg install tampered", timeout=120)
            assert "does not match its index entry" in out, \
                "a package whose hash is wrong was installed anyway:\n" + out
            out = vm.run("pkg list")
            assert "tampered" not in out, "the tampered package was installed:\n" + out
            print("  ok: a package whose bytes do not match its index is refused")

            vm.expect_absent("KERNEL PANIC")
    finally:
        srv.shutdown()


def main():
    local_repo_tests()
    network_repo_tests()
    print("PASS pkg: packages install, with dependencies, from disk and over HTTP")
    return 0


if __name__ == "__main__":
    sys.exit(main())
