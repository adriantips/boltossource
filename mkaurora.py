#!/usr/bin/env python3
"""Paint the default wallpaper: ribbons of light on a deep field.

    mkaurora.py OUT.png [--light] [--size 3840x2160] [--seed N]

Glass is only as good as what is behind it. Blur a grey photograph and you
get a grey panel; blur colour and you get something that looks like glass.
So the default wallpaper is colour on purpose: a few broad bands of light,
each a soft gaussian across its width with a brighter fold along one edge,
added together on a dark gradient, then grained so the JPEG cut by mkwall.py
does not band.

The output is the 4K master; mkwall.py turns it into the per-resolution set
the kernel embeds:

    python mkaurora.py assets/wallpapers/aurora.png
    python mkwall.py assets/wallpapers/aurora.png assets/wallpapers
"""
import math
import sys

import numpy as np
from PIL import Image


def ribbon(X, Y, W, H, cx, amp, freq, phase, tilt, width, fold):
    """Signed distance (px) from a wavy centre line y = f(x), and where along
    the ribbon each pixel is (0..1), for the colour ramp."""
    xs = X / W
    centre = cx * H + tilt * (X - W / 2) + amp * H * np.sin(2 * math.pi * freq * xs + phase) \
        + 0.35 * amp * H * np.sin(2 * math.pi * freq * 2.1 * xs + phase * 1.7)
    d = Y - centre
    return d, xs


def paint(W, H, light=False, seed=7):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    if light:
        top = np.array([0.93, 0.95, 1.00]); bot = np.array([0.98, 0.93, 0.96])
    else:
        top = np.array([0.020, 0.030, 0.085]); bot = np.array([0.075, 0.030, 0.140])
    t = (yy / H)[..., None]
    img = top * (1 - t) + bot * t
    img = img.astype(np.float32)

    # each ribbon: (centre y, amplitude, frequency, phase, tilt, width, colours along it)
    S = W / 3840.0
    ribbons = [
        (0.58, 0.10, 0.85, 0.4, -0.18, 230 * S, [(0.10, 0.75, 1.00), (0.25, 0.35, 1.00), (0.55, 0.20, 0.95)]),
        (0.70, 0.08, 0.65, 2.2, -0.12, 170 * S, [(0.35, 0.20, 1.00), (0.85, 0.25, 0.85), (1.00, 0.45, 0.55)]),
        (0.46, 0.06, 1.10, 4.0, -0.22, 120 * S, [(0.20, 0.95, 0.90), (0.15, 0.55, 1.00), (0.40, 0.30, 1.00)]),
        (0.82, 0.05, 0.50, 1.1, -0.06, 300 * S, [(0.55, 0.15, 0.85), (0.25, 0.20, 0.80), (0.10, 0.45, 0.95)]),
    ]
    acc = np.zeros_like(img)
    for (cy, amp, freq, ph, tilt, width, cols) in ribbons:
        d, xs = ribbon(xx, yy, W, H, cy, amp, freq, ph, tilt, width, 0)
        body = np.exp(-(d / width) ** 2)                      # the soft band
        fold = np.exp(-((d + width * 0.55) / (width * 0.10)) ** 2)   # a bright fold along one edge
        edge = np.exp(-((d - width * 0.9) / (width * 0.35)) ** 2) * 0.35
        # colour ramp along x
        c0, c1, c2 = (np.array(c, dtype=np.float32) for c in cols)
        u = xs[..., None]
        ramp = np.where(u < 0.5, c0 * (1 - u * 2) + c1 * (u * 2), c1 * (1 - (u - 0.5) * 2) + c2 * ((u - 0.5) * 2))
        strength = (0.62 * body + 0.85 * fold + edge)[..., None]
        acc += ramp * strength
    if light:
        # on light, the ribbons are pastel washes laid over paper: mix toward
        # a lightened copy of their own colour by how much ribbon is there
        amt = np.clip(acc.max(axis=2, keepdims=True), 0, 1.6) / 1.6
        tint = np.clip(acc / np.maximum(acc.max(axis=2, keepdims=True), 1e-4), 0, 1)
        pastel = 0.55 * tint + 0.45
        img = img * (1 - 0.62 * amt) + pastel * 0.62 * amt
        img = np.clip(img, 0, 1)
    else:
        # additive light, then a gentle tone curve so the brightest folds
        # roll off instead of clipping
        img = img + acc * 0.85
        img = 1 - np.exp(-img * 1.35)
    # a large soft glow, top right
    gx, gy = W * 0.78, H * 0.18
    glow = np.exp(-(((xx - gx) / (W * 0.35)) ** 2 + ((yy - gy) / (H * 0.45)) ** 2))[..., None]
    img = img + glow * (np.array([0.10, 0.12, 0.30]) if not light else np.array([0.04, 0.03, 0.0]))
    # vignette
    vx = (xx / W - 0.5) * 1.6; vy = (yy / H - 0.5) * 1.6
    vig = (1 - 0.35 * np.clip(vx * vx + vy * vy, 0, 1))[..., None]
    if not light:
        img = img * vig
    # grain, so 8-bit and JPEG do not band the long gradients
    img = img + rng.normal(0, 0.006, img.shape).astype(np.float32)
    img = np.clip(img, 0, 1)
    return (img * 255 + 0.5).astype(np.uint8)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out = argv[1]
    light = "--light" in argv
    size = "3840x2160"
    seed = 7
    for i, a in enumerate(argv):
        if a == "--size": size = argv[i + 1]
        if a == "--seed": seed = int(argv[i + 1])
    W, H = (int(v) for v in size.split("x"))
    Image.fromarray(paint(W, H, light, seed)).save(out)
    print("      %s: %dx%d%s" % (out, W, H, " (light)" if light else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
