#!/usr/bin/env python3
# ===========================================================================
#  BoltOS Mini  -  mini/tools/mklxpak.py
#
#  Packs a folder into an "lx pack": the read-only file tree a Linux program
#  run by Mini's Linux layer (mini/lx_fs.c) sees. Packs live in the card's
#  store (POST /store?name=...) and are unioned at "/", the first pack named
#  winning where two have the same path.
#
#      mklxpak.py OUT.pak DIR [DIR...]      DIR's contents become the root;
#                                           several DIRs are merged, earlier
#                                           ones winning
#      mklxpak.py --list PAK                what is in a pack
#
#  Symbolic links inside DIR are followed (a link to a file becomes that
#  file, sharing its data); identical files are stored once.
#
#  Format, little-endian:
#    header, 64 bytes:
#      0  magic "LXPAK1\0\0"
#      8  u32 entries        12 u32 flags (0)
#     16  u64 index offset   24 u64 names offset   32 u64 names size
#     40  u64 data offset    48 u64 pack size      56 u64 reserved
#    index: one 32-byte entry per file or folder, folders' children together
#    (a breadth-first walk), entry 0 the root:
#      0  u32 name (offset into names: the full path, no leading '/', NUL-ended)
#      4  u32 parent (entry)
#      8  u32 mode (Linux st_mode: 0040755 a folder, 0100644/0100755 a file)
#     12  u32 mtime (seconds)
#     16  u64 file: data offset in the pack / folder: first child's entry
#     24  u64 file: size / folder: how many children
#    names, then the data: each file 512-byte aligned (the card's sectors).
# ===========================================================================
import os, struct, sys, hashlib, stat

ALIGN = 512


def walk(roots):
    """{relpath: (abs path or None for a folder, mode, mtime)} merged over roots."""
    tree = {'': (None, 0o040755, 0)}
    for root in roots:
        root = os.path.abspath(root)
        for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
            dirnames.sort()
            rel = os.path.relpath(dirpath, root)
            rel = '' if rel == '.' else rel.replace(os.sep, '/')
            if rel not in tree:
                tree[rel] = (None, 0o040755, int(os.stat(dirpath).st_mtime))
            for f in sorted(filenames):
                p = os.path.join(dirpath, f)
                r = (rel + '/' + f) if rel else f
                if r in tree:
                    continue
                try:
                    st = os.stat(p)
                except FileNotFoundError:
                    continue                      # a dangling link
                if not stat.S_ISREG(st.st_mode):
                    continue
                mode = 0o100755 if st.st_mode & 0o111 else 0o100644
                tree[r] = (p, mode, int(st.st_mtime))
    return tree


def build(out, roots):
    tree = walk(roots)
    children = {}
    for r in tree:
        if r == '':
            continue
        parent = r.rsplit('/', 1)[0] if '/' in r else ''
        children.setdefault(parent, []).append(r)
    order, idx = [''], {'': 0}
    i = 0
    while i < len(order):                         # breadth first: siblings together
        d = order[i]
        if tree[d][0] is None:
            for c in sorted(children.get(d, [])):
                idx[c] = len(order)
                order.append(c)
        i += 1
    names = bytearray()
    name_off = {}
    for r in order:
        name_off[r] = len(names)
        names += r.encode('utf-8') + b'\0'
    n = len(order)
    index_off = 64
    names_off = index_off + 32 * n
    data_off = (names_off + len(names) + ALIGN - 1) // ALIGN * ALIGN
    # Lay the data out; identical contents share one copy.
    place, by_hash, pos = {}, {}, data_off
    for r in order:
        p, mode, _ = tree[r]
        if p is None:
            continue
        h = hashlib.sha1()
        size = 0
        with open(p, 'rb') as f:
            while True:
                b = f.read(1 << 20)
                if not b:
                    break
                h.update(b)
                size += len(b)
        key = (h.digest(), size)
        if key in by_hash:
            place[r] = (by_hash[key], size, False)
        else:
            by_hash[key] = pos
            place[r] = (pos, size, True)
            pos = (pos + size + ALIGN - 1) // ALIGN * ALIGN
    total = pos
    with open(out + '.tmp', 'wb') as f:
        f.write(struct.pack('<8sIIQQQQQQ', b'LXPAK1\0\0', n, 0, index_off, names_off, len(names),
                            data_off, total, 0))
        for r in order:
            p, mode, mtime = tree[r]
            parent = idx[r.rsplit('/', 1)[0] if '/' in r else ''] if r else 0
            if p is None:
                kids = sorted(children.get(r, []))
                first = idx[kids[0]] if kids else 0
                f.write(struct.pack('<IIIIQQ', name_off[r], parent, mode, mtime & 0xFFFFFFFF, first, len(kids)))
            else:
                off, size, _ = place[r]
                f.write(struct.pack('<IIIIQQ', name_off[r], parent, mode, mtime & 0xFFFFFFFF, off, size))
        f.write(names)
        for r in order:
            p, _, _ = tree[r]
            if p is None or not place[r][2]:
                continue
            off, size, _ = place[r]
            f.seek(off)
            with open(p, 'rb') as s:
                while True:
                    b = s.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
        f.truncate(total)
    os.replace(out + '.tmp', out)
    files = sum(1 for r in order if tree[r][0] is not None)
    print('%s: %d files, %d folders, %.1f MiB' % (out, files, n - files, total / 1048576))


def listing(path):
    with open(path, 'rb') as f:
        h = f.read(64)
        magic, n, _, io, no, ns, do, total, _ = struct.unpack('<8sIIQQQQQQ', h)
        if magic != b'LXPAK1\0\0':
            sys.exit('not an lx pack')
        f.seek(io)
        ents = [struct.unpack('<IIIIQQ', f.read(32)) for _ in range(n)]
        f.seek(no)
        names = f.read(ns)
    for name, parent, mode, mtime, a, b in ents:
        nm = names[name:names.index(b'\0', name)].decode()
        if stat.S_ISDIR(mode):
            print('%7s  /%s/' % ('', nm) if nm else '%7s  /' % '')
        else:
            print('%7d  /%s' % (b, nm))


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == '--list':
        listing(a[1])
    elif len(a) >= 2:
        build(a[0], a[1:])
    else:
        sys.exit(__doc__ or 'usage: mklxpak.py OUT.pak DIR [DIR...] | --list PAK')
