#!/usr/bin/env python3
"""Fuzzing the two places a stranger gets to choose the bytes.

Everything else on the machine parses something a user typed or a disk we
formatted ourselves. Two subsystems parse whatever arrives from outside:

  the network stack   net/tcp.c takes segments before any port is open,
                      net/tls.c and net/x509.c run the record layer and the
                      certificate parser *before* the certificate has been
                      checked, and net/http.c and net/inflate.c take the
                      response afterwards
  the browser         kernel/html.c, kernel/dom.c, kernel/layout.c and
                      kernel/js.c parse whatever a web server sends

All of it runs in ring 0. That is the case for item 99 on the roadmap, and it
is why this is the one test in the suite that does not boot a machine: a fuzzer
wants millions of inputs a minute, and QEMU gives you one boot a minute.

Three harnesses under test/host/, sharing test/host/fuzz.h:

  fuzzweb   html / text / dom / css / layout / select / color / js / jsvm
  fuzznet   http / keepalive / url / cookie / tls / https / cert / certmsg /
            certverify / gzip / inflate, over a fake socket that replays the
            input as though a server had written it
  fuzztcp   listen / listen6 / connect, against the real net/tcp.c with a fake
            IP layer underneath

What stands in for the sanitizers this toolchain does not have:

  the guarded heap    kmalloc gets redzones, a serial and a poison fill; kfree
                      checks the guards, poisons the block and quarantines it;
                      leaks are counted; and allocation can be made to start
                      failing partway through a parse, which is how several of
                      the bugs below were found
  ubsan traps         -fsanitize=undefined,bounds -fsanitize-trap=all, which
                      needs no runtime library and catches the signed overflows
                      and the fixed-array overruns the heap cannot see

Every harness runs its own selftest before fuzzing anything: known input in,
known answer out. That is not ceremony -- the net harness's DNS stub had its
return convention inverted, so every HTTP target exited before parsing a byte
and reported a clean run at two million inputs a second.

    python test/t_fuzz.py                 # the default budget, about a minute
    python test/t_fuzz.py --iters 200000  # a soak
    python test/t_fuzz.py --seed 7        # a different sequence

A crash prints the seed, the iteration, the input as hex, and the faulting
address with the addr2line command to resolve it, and writes the input to
test/out/crash-TARGET-SEED-ITER.bin. Put that file in test/corpus/ and it is
replayed on every run from then on.
"""
import os, shutil, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import ROOT

ROOT = Path(ROOT)
OUT = ROOT / "test" / "out"
HOST = ROOT / "test" / "host"
CORPUS = ROOT / "test" / "corpus"

# -fno-builtin so libc/string.c's own memcpy is not compiled into a call to
# itself; no -ffreestanding, which on this toolchain quietly removes the CRT
# hook that turns a hardware fault into SIGSEGV and leaves the harness unable
# to report its own crashes.
CFLAGS = [
    "-std=c11", "-O1", "-gdwarf-4", "-Wall", "-Wextra",
    "-Wno-inconsistent-dllimport", "-fno-builtin",
    "-fsanitize=undefined,bounds", "-fsanitize-trap=all",
    "-iquote", str(ROOT / "include"), "-iquote", str(HOST),
]

LIBC = ["libc/string.c", "libc/string_ext.c"]

HARNESSES = {
    "web": ["test/host/fuzzweb.c", "kernel/html.c", "kernel/dom.c",
            "kernel/layout.c", "kernel/js.c"] + LIBC,
    "net": ["test/host/fuzznet.c", "net/tls.c", "net/http.c", "net/x509.c",
            "net/crypto.c", "net/rsa.c", "net/p256.c", "net/p384.c",
            "net/inflate.c"] + LIBC,
    "tcp": ["test/host/fuzztcp.c", "net/tcp.c", "net/netaddr.c"] + LIBC,
}

# The default budget is picked so the whole thing is about a minute: the JS
# engine and the TLS handshake are two orders of magnitude slower per input
# than a colour parser, so a flat count would spend all its time in two targets.
BUDGET = {"web": 4000, "net": 4000, "tcp": 8000}


def find_cc():
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    return cc


def build(cc, name):
    exe = OUT / ("fuzz%s.exe" % name if os.name == "nt" else "fuzz" + name)
    cmd = [cc] + CFLAGS + [str(ROOT / s) for s in HARNESSES[name]] + ["-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise RuntimeError("fuzz%s did not build" % name)
    return exe


def targets_of(exe):
    r = subprocess.run([str(exe), "--list"], capture_output=True, text=True, cwd=str(ROOT))
    return [t for t in r.stdout.split() if t]


def corpus_files(name, target):
    """Saved crashers, replayed before anything is mutated. A fuzzer that does
    not remember what it found is a fuzzer that finds it again next year."""
    d = CORPUS / name / target
    return sorted(d.glob("*.bin")) if d.is_dir() else []


def run(exe, name, target, seed, iters):
    cases = corpus_files(name, target)
    if cases:
        print("     %-10s %d saved case%s replayed" % (target, len(cases), "" if len(cases) == 1 else "s"))
    for case in cases:
        # The filename may carry the allocation count the crash needed:
        # <anything>.oom<N>.bin.
        args = [str(exe), "--target", target, "--case", str(case)]
        stem = case.name
        if ".oom" in stem:
            args += ["--oom", stem.split(".oom")[1].split(".")[0]]
        r = subprocess.run(args, capture_output=True, text=True, cwd=str(ROOT))
        if r.returncode != 0:
            sys.stderr.write(r.stdout + r.stderr)
            raise AssertionError("regression: %s/%s still crashes on %s" % (name, target, case.name))

    r = subprocess.run([str(exe), "--target", target, "--seed", str(seed),
                        "--iters", str(iters)],
                       capture_output=True, text=True, cwd=str(ROOT), timeout=1800)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise AssertionError("%s/%s failed -- see the saved case above" % (name, target))


def main(argv):
    seed = 1
    scale = None
    only = []
    i = 1
    while i < len(argv):
        if argv[i] == "--seed" and i + 1 < len(argv):
            seed = int(argv[i + 1]); i += 2
        elif argv[i] == "--iters" and i + 1 < len(argv):
            scale = int(argv[i + 1]); i += 2
        else:
            only.append(argv[i]); i += 1

    cc = find_cc()
    if not cc:
        print("SKIP fuzz: no host C compiler (set BOLTOS_HOST_CC)")
        return 0
    OUT.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    for name in ("web", "net", "tcp"):
        if only and name not in only:
            continue
        exe = build(cc, name)
        # Always: the selftest runs on startup, so this also proves the
        # harness is wired to the code it claims to be fuzzing.
        r = subprocess.run([str(exe), "--selftest"], capture_output=True, text=True, cwd=str(ROOT))
        sys.stdout.write(r.stdout)
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            raise AssertionError("fuzz%s is not reaching its targets" % name)
        iters = scale if scale is not None else BUDGET[name]
        for target in targets_of(exe):
            run(exe, name, target, seed, iters)

    print("PASS fuzz: %d targets, seed %d, %.0fs" %
          (sum(len(targets_of(OUT / ("fuzz%s.exe" % n if os.name == "nt" else "fuzz" + n)))
               for n in ("web", "net", "tcp") if not only or n in only),
           seed, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
