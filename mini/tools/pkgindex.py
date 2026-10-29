#!/usr/bin/env python3
"""Write the package list boltos.dev serves at /pkg/index.json, which the
Packages app on BoltOS Mini reads (mini/pkg.c). One entry per built .bpk:
what its pkg.conf says about it, its size and SHA-256, and where it is.

    pkgindex.py OUT.json BUILD_PKG_DIR mini/pkg
"""
import hashlib, json, os, sys

BASE = "https://boltos.dev/pkg/"


def conf(path):
    d = {}
    for line in open(path, encoding="utf-8"):
        if "=" in line:
            k, v = line.rstrip("\n").split("=", 1)
            d[k] = v
    return d


def main(out, built, src):
    pkgs = []
    for name in sorted(os.listdir(src)):
        c = os.path.join(src, name, "pkg.conf")
        if not os.path.isfile(c):
            continue
        k = conf(c)
        f = os.path.join(built, "%s-%s.bpk" % (k["id"], k["version"]))
        data = open(f, "rb").read()
        pkgs.append({
            "id": k["id"], "name": k["name"], "version": k["version"], "abi": 1,
            "summary": k.get("summary", ""), "about": k.get("about", ""), "needs": k.get("needs", ""),
            "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "url": BASE + os.path.basename(f),
        })
    json.dump({"packages": pkgs}, open(out, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    print("      %s: %d package%s" % (out, len(pkgs), "" if len(pkgs) == 1 else "s"))


if __name__ == "__main__":
    main(*sys.argv[1:4])
