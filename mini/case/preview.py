"""Shaded renders of the assembled case: a small numpy z-buffer rasteriser (no GPU, no OpenGL)."""
import os
import numpy as np

CASE = {"deck": "#d9d4cc", "hub_cover": "#bfb8ad", "tower": "#d9d4cc", "lid": "#c9c3b9",
        "well_lid": "#9fb0c6", "pi_sled": "#8fbf86", "mast": "#c9c3b9",
        "screen_bezel": "#3a3d42", "screen_back": "#55595f"}
PARTS = {"keyboard": "#2f6fb0", "usb hub": "#c0504d", "power bank": "#e3a32b",
         "orange pi": "#2e8b57", "lcd": "#77c7f2", "keystone": "#8e5fb5"}

def hex2rgb(h):
    h = h.lstrip("#"); return np.array([int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)])

def tris(m):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3].astype(np.float64)
    return v[np.asarray(mesh.tri_verts)]

def render_view(items, el, az, size=900, path=None):
    from PIL import Image
    a, e = np.radians(az), np.radians(el)
    # camera basis: looking from direction d toward the origin
    d = np.array([np.cos(e) * np.sin(a), -np.cos(e) * np.cos(a), np.sin(e)])   # toward camera
    right = np.cross([0, 0, 1], d); right /= np.linalg.norm(right)
    up = np.cross(d, right)
    light = 0.7 * d + 0.5 * up + 0.3 * right; light /= np.linalg.norm(light)
    T, C = [], []
    for m, col in items:
        t = tris(m)
        n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
        s = 0.35 + 0.65 * np.abs(n @ light)
        T.append(t); C.append(np.clip(hex2rgb(col)[None] * s[:, None], 0, 1))
    T = np.concatenate(T); C = np.concatenate(C)
    P = np.stack([T @ right, T @ up, T @ d], axis=-1)          # screen x, y, depth (bigger = nearer)
    lo = P[..., :2].reshape(-1, 2).min(0); hi = P[..., :2].reshape(-1, 2).max(0)
    scale = (size * 0.9) / (hi - lo).max()
    off = (size - (hi - lo) * scale) / 2
    P[..., :2] = (P[..., :2] - lo) * scale + off
    P[..., 1] = size - P[..., 1]
    zbuf = np.full((size, size), -np.inf); img = np.ones((size, size, 3))
    for (p0, p1, p2), c in zip(P, C):
        x0 = int(max(min(p0[0], p1[0], p2[0]), 0)); x1 = int(min(max(p0[0], p1[0], p2[0]) + 1, size))
        y0 = int(max(min(p0[1], p1[1], p2[1]), 0)); y1 = int(min(max(p0[1], p1[1], p2[1]) + 1, size))
        if x1 <= x0 or y1 <= y0: continue
        den = (p1[1] - p2[1]) * (p0[0] - p2[0]) + (p2[0] - p1[0]) * (p0[1] - p2[1])
        if abs(den) < 1e-9: continue
        xs, ys = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        w0 = ((p1[1] - p2[1]) * (xs - p2[0]) + (p2[0] - p1[0]) * (ys - p2[1])) / den
        w1 = ((p2[1] - p0[1]) * (xs - p2[0]) + (p0[0] - p2[0]) * (ys - p2[1])) / den
        w2 = 1 - w0 - w1
        inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
        if not inside.any(): continue
        z = w0 * p0[2] + w1 * p1[2] + w2 * p2[2]
        zb = zbuf[y0:y1, x0:x1]
        upd = inside & (z > zb)
        zb[upd] = z[upd]
        img[y0:y1, x0:x1][upd] = c
    # outline: darken depth discontinuities
    zf = np.where(np.isfinite(zbuf), zbuf, zbuf[np.isfinite(zbuf)].min() - 50)
    edge = (np.abs(np.diff(zf, axis=0, prepend=zf[:1])) > 3) | (np.abs(np.diff(zf, axis=1, prepend=zf[:, :1])) > 3)
    img[edge] *= 0.35
    Image.fromarray((img * 255).astype(np.uint8)).save(path)

def render(world, dummies, outdir):
    os.makedirs(outdir, exist_ok=True)
    full = [(m, CASE[k]) for k, m in world.items()] + [(m, PARTS[k]) for k, m in dummies.items()]
    render_view(full, 25, -35, path=os.path.join(outdir, "assembled_front.png"))
    render_view(full, 25, 145, path=os.path.join(outdir, "assembled_back.png"))
    render_view(full, 0, 90, path=os.path.join(outdir, "assembled_side.png"))
    inside = [(m, CASE[k]) for k, m in world.items() if k in ("deck", "tower", "pi_sled")] + \
             [(m, PARTS[k]) for k, m in dummies.items() if k != "lcd"]
    render_view(inside, 60, -25, path=os.path.join(outdir, "inside_top.png"))
    render_view(inside, -60, -25, path=os.path.join(outdir, "underside.png"))
