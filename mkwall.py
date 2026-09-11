#!/usr/bin/env python3
"""Cut the desktop wallpaper down to one render per resolution BoltOS offers.

BoltOS reprograms the GPU to a real native mode -- there is no upscaler in the
path -- so a single wallpaper would be stretched by the compositor's nearest
neighbour sampler at four of the five settings, and nearest neighbour on a
photograph looks exactly as bad as it sounds. One file per mode fixes that: the
compositor's stretch loop becomes a 1:1 copy.

The 4K master is 16:9. Every mode is 16:9 except 1024x768, which is 4:3, so
that one gets a centre crop before the resize -- squashing a mountain range to
three quarters of its width is worse than losing the outer eighth of the sky.

JPEG, not PNG, and not out of laziness. The wallpapers are embedded in the
kernel image and decoded by kernel/image.c, and a 4K PNG costs ~99 MiB of heap
to decode there (inflate buffer + unfiltered rows + ARGB output, all live at
once) against a 96 MiB kheap. The JPEG path peaks near 46 MiB and the whole set
ships in 1.5 MB instead of 13.5 MB.

    mkwall.py MASTER.png OUTDIR
"""
import os
import re
import sys

QUALITY = 88          # high enough that the sky gradient stays free of banding
RES_TABLE = "kernel/settings.c"


def resolutions(path):
    """Read the mode list out of settings.c so this never drifts from the OS.

    The table is the single source of truth for what the Settings app offers;
    parsing it beats a second copy here that someone forgets to update."""
    src = open(path, encoding="utf-8", errors="replace").read()
    body = re.search(r"res_tab\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not body:
        raise SystemExit("mkwall: no res_tab[] in %s -- did it get renamed?" % path)
    out = [(int(w), int(h)) for w, h in
           re.findall(r'\{\s*"[^"]*"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', body.group(1))]
    if not out:
        raise SystemExit("mkwall: res_tab[] in %s parsed empty" % path)
    return out


def fit(im, w, h, Image):
    """Centre-crop to the target aspect, then resample. Never distorts."""
    sw, sh = im.size
    if sw * h > sh * w:                       # source wider: trim left/right
        cw, ch = sh * w // h, sh
    else:                                     # source taller: trim top/bottom
        cw, ch = sw, sw * h // w
    box = ((sw - cw) // 2, (sh - ch) // 2, (sw - cw) // 2 + cw, (sh - ch) // 2 + ch)
    return im.resize((w, h), Image.LANCZOS, box=box)


def main(argv):
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("mkwall: needs Pillow (pip install pillow)")

    master = argv[1] if len(argv) > 1 else "assets/wallpapers/mountain.png"
    outdir = argv[2] if len(argv) > 2 else os.path.dirname(master) or "."
    stem = os.path.splitext(os.path.basename(master))[0]
    os.makedirs(outdir, exist_ok=True)

    im = Image.open(master).convert("RGB")
    for w, h in resolutions(RES_TABLE):
        path = os.path.join(outdir, "%s-%dx%d.jpg" % (stem, w, h))
        # 4:2:0 and baseline: the two things kernel/image.c's JPEG decoder can
        # actually read back. Progressive would fail at decode time, silently,
        # as a procedural gradient on someone's desktop.
        fit(im, w, h, Image).save(path, "JPEG", quality=QUALITY, optimize=True,
                                  progressive=False, subsampling="4:2:0")
        print("      %s: %dx%d, %d bytes"
              % (os.path.basename(path), w, h, os.path.getsize(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
