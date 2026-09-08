#!/usr/bin/env python3
"""Run the BoltOS QEMU regression suite.

    python test/run_all.py            # every test
    python test/run_all.py boot fs    # only tests whose name matches

Each test is a standalone script in this directory named t_*.py that boots a
real BoltOS in QEMU and asserts on the serial log. They run one at a time (each
one owns a whole VM), and a test that cannot run in this environment prints
SKIP and exits 0 rather than failing the suite.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))

# Cheapest and most fundamental first, so a broken boot is reported in a minute
# rather than after the slow durability runs.
ORDER = ["t_boot.py", "t_bootmenu.py", "t_uefi.py", "t_fs.py", "t_persist.py"]


def discover(filters):
    names = sorted(f for f in os.listdir(HERE)
                   if f.startswith("t_") and f.endswith(".py"))
    names.sort(key=lambda n: (ORDER.index(n) if n in ORDER else len(ORDER), n))
    if filters:
        names = [n for n in names if any(f in n for f in filters)]
    return names


def main(argv):
    tests = discover(argv[1:])
    if not tests:
        print("no tests matched")
        return 1
    if not os.path.exists(os.path.join(os.path.dirname(HERE), "iso", "os.img")):
        print("iso/os.img missing -- run build.sh first")
        return 1

    results = []
    for name in tests:
        print("=" * 70)
        print("RUN  %s" % name)
        print("=" * 70)
        t0 = time.time()
        sys.stdout.flush()
        # Children inherit this stdout. When the suite's own output is a file
        # (CI, or a background run) Python buffers both ends, and a failing
        # test's diagnostics are then lost behind the summary -- which is the
        # one moment they are actually needed.
        env = dict(os.environ)
        env["PYTHONUNBUFFERED"] = "1"
        rc = subprocess.call([sys.executable, "-u", os.path.join(HERE, name)], env=env)
        sys.stdout.flush()
        dt = time.time() - t0
        results.append((name, rc, dt))
        print("%-16s %s  (%.0fs)" % (name, "ok" if rc == 0 else "FAILED rc=%d" % rc, dt))

    print("=" * 70)
    bad = [n for n, rc, _ in results if rc != 0]
    for name, rc, dt in results:
        print("  %-16s %-8s %5.0fs" % (name, "ok" if rc == 0 else "FAIL", dt))
    print("%d/%d passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
