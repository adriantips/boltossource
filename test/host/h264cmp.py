#!/usr/bin/env python3
"""Compare two raw I420 files frame by frame: ours (test/host/h264test.c) and
ffmpeg's. Prints the first frame that differs, which plane, and which
macroblock -- the place to start looking.

    h264cmp.py OURS.yuv REF.yuv WIDTH HEIGHT
"""
import sys
a, b, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
fs = w * h * 3 // 2
A = open(a, 'rb').read(); B = open(b, 'rb').read()
n = min(len(A), len(B)) // fs
print('%d frames ours, %d frames reference, comparing %d' % (len(A) // fs, len(B) // fs, n))
for i in range(n):
    fa, fb = A[i * fs:(i + 1) * fs], B[i * fs:(i + 1) * fs]
    if fa == fb:
        continue
    for name, off, pw, ph, mb in (('Y', 0, w, h, 16), ('U', w * h, w // 2, h // 2, 8), ('V', w * h * 5 // 4, w // 2, h // 2, 8)):
        pa, pb = fa[off:off + pw * ph], fb[off:off + pw * ph]
        if pa != pb:
            k = next(j for j in range(len(pa)) if pa[j] != pb[j])
            y, x = divmod(k, pw)
            bad = sum(1 for j in range(len(pa)) if pa[j] != pb[j])
            print('frame %d differs: plane %s, first at (%d,%d) = MB (%d,%d), ours %d ref %d; %d samples differ' %
                  (i, name, x, y, x // mb, y // mb, pa[k], pb[k], bad))
            break
    sys.exit(1)
print('all %d frames match' % n)
