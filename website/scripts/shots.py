"""Turn raw QEMU screendumps into the site's images.

    python scripts/shots.py SRC_DIR [SRC_DIR ...]

Later directories win over earlier ones for the same name. Writes
public/shots/<key>.webp (full size), public/shots/thumb/<key>.webp (960 wide)
and src/data/shots.json (key -> [w, h]), which is what decides whether a
screenshot exists as far as the pages are concerned.

The historical June 2026 images and boltcraft's own screenshot are picked up
from the repository root, so they do not need to be copied anywhere first.

Some images have no source left to regenerate them from, and their WebP in
public/shots is the master copy: the June desktop (imageshowcase.png became
the glass desktop in d1505f1), the text-mode boot menu, and every v1_* --
the September desktop, kept for the before/after on /revamp. KEEP carries
those into the manifest as they are.
"""
import json, os, sys
from PIL import Image

SITE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(SITE)
OUT = os.path.join(SITE, "public", "shots")
THUMB = os.path.join(OUT, "thumb")
os.makedirs(THUMB, exist_ok=True)

# renames for the second capture pass, and files from the repo itself
RENAME = {"login_1024": "login", "bootmenu_227": "bootmenu"}
SKIP = {"login_check", "bootmenu_225", "doom_play2", "term_check", "keys_check", "ptr_check", "ptr_test", "ptr_test2"}
KEEP = ("legacy_", "v1_", "bootmenu")
EXTRA = {
    "legacy_icons": os.path.join(REPO, "m.png"),
    "legacy_login": os.path.join(REPO, "boot.png"),
    "legacy_nssite": os.path.join(REPO, "nssite.png"),
    "legacy_nsrender": os.path.join(REPO, "nsrender.png"),
    "boltcraft_shot": os.path.join(REPO, "boltcraft", "shot.png"),
}

src = {}
for d in sys.argv[1:]:
    for f in sorted(os.listdir(d)):
        if not f.endswith(".png"):
            continue
        k = f[:-4]
        if k in SKIP:
            continue
        src[RENAME.get(k, k)] = os.path.join(d, f)
for k, p in EXTRA.items():
    if os.path.exists(p):
        src[k] = p

manifest = {}
for f in sorted(os.listdir(OUT)):
    k = f[:-5]
    if f.endswith(".webp") and k.startswith(KEEP) and k not in src:
        with Image.open(os.path.join(OUT, f)) as im:
            manifest[k] = [im.width, im.height]
        print(k, "(kept)")
for k, p in sorted(src.items()):
    im = Image.open(p).convert("RGB")
    # text-mode captures (the boot menu) are 720x400; double them with
    # nearest-neighbour so the glyphs stay hard-edged instead of smeared
    if im.width < 1000 and k in ("bootmenu",):
        im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
    im.save(os.path.join(OUT, k + ".webp"), "WEBP", quality=86, method=6)
    tw = min(960, im.width)
    th = round(im.height * tw / im.width)
    im.resize((tw, th), Image.LANCZOS).save(os.path.join(THUMB, k + ".webp"), "WEBP", quality=80, method=6)
    manifest[k] = [im.width, im.height]
    print(k, im.size)

# <key>_win: each windowed capture cropped to its window, found by diffing it
# against the empty desktop. Rows and columns need more than a handful of
# changed pixels to count, which drops the cursor and the taskbar clock.
from PIL import ImageChops
if "desktop" in src:
    desk = Image.open(src["desktop"]).convert("RGB")
    for k, p in sorted(src.items()):
        if k.endswith("_max") or k.startswith(("legacy", "v1_", "desktop", "boot", "login", "lock", "tiled", "doom_play", "strike_play", "boltcraft_shot", "start", "quick", "notif", "terminal")):
            continue
        im = Image.open(p).convert("RGB")
        if im.size != desk.size:
            continue
        # leave the dock out of the search: it grows an icon for every open
        # app, which would drag every crop down to the bottom of the screen
        H = im.height - 80
        diff = ImageChops.difference(im.crop((0, 0, im.width, H)), desk.crop((0, 0, im.width, H))).convert("L")
        mask = diff.point(lambda v: 255 if v > 24 else 0)
        px = mask.load()
        cols = [x for x in range(0, im.width, 2) if sum(1 for y in range(0, H, 4) if px[x, y]) > 12]
        rows = [y for y in range(0, H, 2) if sum(1 for x in range(0, im.width, 4) if px[x, y]) > 12]
        if not cols or not rows:
            continue
        x0, x1, y0, y1 = cols[0], cols[-1], rows[0], rows[-1]
        if (x1 - x0) < 120 or (y1 - y0) < 120:
            continue
        pad = 56
        x0, y0, x1, y1 = x0 - pad, y0 - pad, x1 + pad, y1 + pad
        # grow to 16:10 around the window, then slide back inside the frame
        w, h = x1 - x0, y1 - y0
        if w / h < 1.6:
            w = int(h * 1.6)
        else:
            h = int(w / 1.6)
        w, h = min(w, im.width), min(h, im.height)
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        x0 = max(0, min(im.width - w, cx - w // 2))
        y0 = max(0, min(im.height - h, cy - h // 2))
        crop = im.crop((x0, y0, x0 + w, y0 + h))
        key = k + "_win"
        crop.save(os.path.join(OUT, key + ".webp"), "WEBP", quality=88, method=6)
        tw = min(960, crop.width)
        crop.resize((tw, round(crop.height * tw / crop.width)), Image.LANCZOS).save(os.path.join(THUMB, key + ".webp"), "WEBP", quality=82, method=6)
        manifest[key] = [crop.width, crop.height]
        print(key, crop.size)

with open(os.path.join(SITE, "src", "data", "shots.json"), "w") as f:
    json.dump(manifest, f, indent=0, sort_keys=True)
print(len(manifest), "images")
