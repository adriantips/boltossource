#!/usr/bin/env python3
"""Make a BoltOS Mini package (.bpk) from a package linked as an AArch64
shared object at address 0 (mini/pkg.h describes the format).

The image is every PT_LOAD segment laid out at its address. Each dynamic
relocation becomes one of two things:

  * an address of the package's own (R_AARCH64_RELATIVE, or a symbol the
    package defines): the offset goes in the list the loader adds the load
    address to, and the value relative to 0 goes in the image;
  * a kernel symbol (a symbol the package uses but doesn't define): the
    offset and the name go in the import list, the addend in the image.

Anything else is refused, and so is an import that isn't in the kernel's
export table (--exports mini/exports.c), so a package that would not load
is never made.

    mkpkg.py IN.so OUT.bpk --id localai --name "BoltOS Local AI" --version 1.0.0
             --exports mini/exports.c
"""
import argparse, re, struct, sys

R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027
PT_LOAD = 1
SHT_RELA, SHT_DYNSYM, SHT_SYMTAB = 4, 11, 2
HEAD = 256


def die(msg):
    raise SystemExit("mkpkg: " + msg)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--id", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--exports", required=True)
    ap.add_argument("--abi", type=int, default=1)
    a = ap.parse_args(argv)

    exports = set(re.findall(r"\bX\((\w+)\)", open(a.exports).read()))
    if not exports:
        die("no exports found in %s" % a.exports)

    f = open(a.elf, "rb").read()
    if f[:4] != b"\x7fELF" or f[4] != 2 or f[5] != 1:
        die("%s is not a 64-bit little-endian ELF" % a.elf)
    (e_type, e_machine, _, e_entry, e_phoff, e_shoff, _, _, e_phentsize, e_phnum,
     e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", f, 16)
    if e_machine != 183:
        die("not AArch64")

    # The image: the loadable segments at their addresses.
    segs = []
    for i in range(e_phnum):
        p_type, p_flags, p_off, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", f, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD:
            segs.append((p_vaddr, p_off, p_filesz, p_memsz))
    if not segs or min(s[0] for s in segs) != 0:
        die("the package must be linked at 0")
    file_end = max(v + fs for v, _, fs, _ in segs)
    mem_end = max(v + ms for v, _, _, ms in segs)
    img = bytearray(file_end)
    for v, off, fs, _ in segs:
        img[v:v + fs] = f[off:off + fs]
    mem_size = (mem_end + 4095) & ~4095

    # Sections: the relocation tables and the dynamic symbols they refer to.
    secs = []
    for i in range(e_shnum):
        secs.append(struct.unpack_from("<IIQQQQIIQQ", f, e_shoff + i * e_shentsize))
    shstr = secs[e_shstrndx]
    def secname(s):
        o = shstr[4] + s[0]
        return f[o:f.index(b"\0", o)].decode()

    def symbols(s):
        strtab = secs[s[6]]
        out = []
        for k in range(s[5] // 24):
            st_name, st_info, _, st_shndx, st_value, _ = struct.unpack_from("<IBBHQQ", f, s[4] + 24 * k)
            o = strtab[4] + st_name
            out.append((f[o:f.index(b"\0", o)].decode(), st_shndx, st_value))
        return out

    entry = None
    for s in secs:
        if s[1] in (SHT_SYMTAB, SHT_DYNSYM):
            for name, shndx, value in symbols(s):
                if name == "pkg_main" and shndx:
                    entry = value
    if entry is None:
        die("pkg_main isn't defined")

    rel, imp, missing = [], [], set()
    for s in secs:
        if s[1] != SHT_RELA:
            continue
        syms = symbols(secs[s[6]]) if s[6] else []
        for k in range(s[5] // 24):
            r_off, r_info, r_add = struct.unpack_from("<QQq", f, s[4] + 24 * k)
            typ, si = r_info & 0xFFFFFFFF, r_info >> 32
            if r_off + 8 > len(img):
                img.extend(b"\0" * (r_off + 8 - len(img)))
            if typ == R_RELATIVE:
                struct.pack_into("<q", img, r_off, r_add)
                rel.append(r_off)
            elif typ in (R_ABS64, R_GLOB_DAT, R_JUMP_SLOT):
                name, shndx, value = syms[si]
                if shndx:                      # its own symbol
                    struct.pack_into("<q", img, r_off, value + r_add)
                    rel.append(r_off)
                else:
                    if name not in exports:
                        missing.add(name)
                    struct.pack_into("<q", img, r_off, r_add)
                    imp.append((r_off, name))
            else:
                die("relocation type %d in %s can't be loaded (build with -fpic)" % (typ, secname(s)))
    if missing:
        die("the kernel doesn't export: %s (add them to %s)" % (", ".join(sorted(missing)), a.exports))
    if len(img) > mem_size:
        mem_size = (len(img) + 4095) & ~4095

    rel_b = b"".join(struct.pack("<I", o) for o in sorted(rel))
    imp_b = b"".join(struct.pack("<IB", o, len(n)) + n.encode() for o, n in sorted(imp))
    def fixed(s, n):
        b = s.encode()[:n - 1]
        return b + b"\0" * (n - len(b))
    head = (b"BPK1" + struct.pack("<9I", HEAD, a.abi, 0, len(img), mem_size, entry, len(rel), len(imp), len(imp_b))
            + fixed(a.id, 32) + fixed(a.name, 48) + fixed(a.version, 16))
    head += b"\0" * (HEAD - len(head))
    out = head + bytes(img) + rel_b + imp_b
    open(a.out, "wb").write(out)
    print("      %s: %d KiB image, %d KiB in memory, %d own addresses, %d kernel symbols (%d distinct)"
          % (a.out, len(img) >> 10, mem_size >> 10, len(rel), len(imp), len(set(n for _, n in imp))))


if __name__ == "__main__":
    main(sys.argv[1:])
