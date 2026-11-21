#!/usr/bin/env python3
"""
BoltOS Mini desk case: Orange Pi Zero3 + power bank + mini keyboard (with touchpad)
+ 3.5" HDMI LCD on a tilting mast + 4-port USB hub + an Ethernet cable well.

    pip install manifold3d numpy matplotlib
    python3 case.py            # writes stl/*.stl and preview/*.png

Every size is in millimetres. MEASURE YOUR PARTS and change the numbers in the
"Your parts" block, then run again. Each printed part fits a 220 x 220 mm bed.

Coordinates of the assembled case: x = left -> right, y = front (you) -> back,
z = up. The keyboard deck is y 0..DECK_D, the tower sits right behind it.
"""
import math, os, struct, sys
import numpy as np
from manifold3d import Manifold as M, OpType

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- Your parts
KB_W, KB_D, KB_T = 145.0, 55.0, 9.5      # mini keyboard w/ touchpad: 14.5 x 5.5 cm, under 1 cm thick
KB_CLR = 1.0                             # gap around the keyboard in its pocket
KB_SINK = KB_T - 2.5                     # pocket depth: keyboard stands 2.5 mm proud of the deck

HUB_L, HUB_W, HUB_H = 105.0, 47.5, 13.0  # USB hub: 10.5 x 4.75 cm (height assumed); ports face right
HUB_PORTS_L = 85.0                       # length of the row of ports along the hub

BANK_L, BANK_H, BANK_T = 150.0, 70.0, 16.0  # power bank 15 x 7 cm (thickness assumed), on its long edge

PI_W, PI_D = 55.0, 50.0                  # Orange Pi Zero3 board
PI_HOLES = [(3.0, 3.0), (52.0, 3.0), (3.0, 47.0), (52.0, 47.0)]  # from board corner
PI_SCREW_PILOT = 2.2                     # M2.5 self-tapping into the standoffs

# 3.5" HDMI LCD, LCDwiki MPI3508 (numbers from its LCM outline drawing), used in landscape:
# HDMI socket on the bottom long edge, micro-USB power on the left short edge, 26-pin header at the top.
LCD_W, LCD_H = 85.51, 56.0               # glass outline
LCD_PANEL_T = 3.7                        # glass + backlight
LCD_PCB_L, LCD_PCB_H, LCD_PCB_T = 75.0, 60.6, 1.6   # PCB starts at the left (micro-USB) end of the glass
LCD_DEPTH = 20.86                        # front of the glass to the back of the pin header
LCD_T = LCD_PANEL_T + LCD_PCB_T          # front of the glass to the back of the PCB
VIEW_W, VIEW_H = 74.5, 50.0              # window over the 73.44 x 48.96 active area
VIEW_DX, VIEW_DY = 3.5, 0.0              # active area sits 3.5 mm off centre, away from the USB end
HDMI_X = (20.0, 36.0)                    # HDMI socket along the bottom edge, from the glass's left end
USB_Y = 10.3                             # micro-USB centre up the left edge, from the glass's bottom

# Keystone RJ45 coupler (standard keystone: ~14.9 x 16.5 mm cut-out, 2 mm panel)
KEY_W, KEY_H, KEY_PANEL = 14.9, 16.5, 2.0

# ---------------------------------------------------------------- Case sizes
WALL = 3.0
KB_X0 = 4.0               # keyboard pocket left edge
# width / deck depth grow on their own if your keyboard or hub is bigger
W = max(200.0, KB_X0 + KB_W + 2 * KB_CLR + 11 + HUB_W + 3)
KB_Y0 = 20.0              # palm rest in front of the keyboard
DECK_D = max(120.0, KB_Y0 + KB_D + 2 * KB_CLR + 10, HUB_L + 30)
DECK_FRONT_H = max(18.0, 2.0 + HUB_H + 1 + 4)   # tall enough over the hub
DECK_BACK_H = DECK_FRONT_H + DECK_D * math.tan(math.radians(6))   # 6 degree keyboard tilt
TOWER_D = 110.0
TOWER_H = 87.0            # tower walls; the lid adds LID_T on top
LID_T = 3.0
WELL_W = 73.0             # Ethernet cable well (front-right of the tower), inner size
WELL_D = 78.0
DRUM_D = 32.0             # post you wrap the cable around
MAST_H = 85.0             # pivot height above the lid
SCREEN_TILT = 15.0        # preview tilt (the real one is set by the pivot screws)
POD_W, POD_H = 108.0, 84.0             # screen pod outside
POD_D = max(22.0, 2.5 + LCD_DEPTH + 1.5)  # pod depth without the back cover
POD_COVER_T = 3.0

M3_PILOT, M3_CLEAR, M3_HEAD = 2.6, 3.4, 6.4   # M3 screws driven straight into the plastic
M4_PILOT, M4_CLEAR = 3.4, 4.4                  # M4 pivot screws (also cut their own thread)
SEG = 48

# ---------------------------------------------------------------- helpers
def box(x0, y0, z0, x1, y1, z1):
    return M.cube((x1 - x0, y1 - y0, z1 - z0)).translate((x0, y0, z0))

def cyl(cx, cy, z0, z1, d, seg=SEG):
    return M.cylinder(z1 - z0, d / 2, circular_segments=seg).translate((cx, cy, z0))

def cyl_x(x0, x1, cy, cz, d, seg=SEG):
    return M.cylinder(x1 - x0, d / 2, circular_segments=seg).rotate((0, 90, 0)).translate((x0, cy, cz))

def cyl_y(y0, y1, cx, cz, d, seg=SEG):
    return M.cylinder(y1 - y0, d / 2, circular_segments=seg).rotate((-90, 0, 0)).translate((cx, y0, cz))

def union(parts):
    parts = [p for p in parts if p is not None]
    return M.batch_boolean(parts, OpType.Add) if len(parts) > 1 else parts[0]

def rbox(x0, y0, z0, x1, y1, z1, r):
    """Box with rounded vertical edges; r = radius or (r_x0y0, r_x1y0, r_x1y1, r_x0y1)."""
    rs = r if isinstance(r, (tuple, list)) else (r, r, r, r)
    pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    sx = [1, -1, -1, 1]; sy = [1, 1, -1, -1]
    bits = []
    for (px, py), rr, a, b in zip(pts, rs, sx, sy):
        if rr <= 0.01:
            e = 0.01
            bits.append(box(px + (0 if a > 0 else -e), py + (0 if b > 0 else -e), z0,
                            px + (e if a > 0 else 0), py + (e if b > 0 else 0), z1))
        else:
            bits.append(cyl(px + a * rr, py + b * rr, z0, z1, 2 * rr, 64))
    return M.batch_hull(bits)

def countersink(x, y, z_top, d=M3_CLEAR, head=M3_HEAD, depth=40):
    """Through hole with a 90-degree countersink whose wide end is at z_top."""
    cone = M.cylinder(head / 2, d / 2, head / 2, circular_segments=SEG).translate((x, y, z_top - head / 2))
    return cyl(x, y, z_top - depth, z_top + 1, d) + cone + cyl(x, y, z_top, z_top + 5, head)

# ---------------------------------------------------------------- derived layout
TY0 = DECK_D                          # tower front face
TY1 = DECK_D + TOWER_D                # tower back face
SLOPE = math.degrees(math.atan((DECK_BACK_H - DECK_FRONT_H) / DECK_D))
KB_CX = KB_X0 + (KB_W + 2 * KB_CLR) / 2   # keyboard centre: the screen lines up with it

# tower interior zones
IN_X0, IN_X1 = WALL, W - WALL
IN_Y0, IN_Y1 = TY0 + WALL, TY1 - WALL
WELL_X0 = IN_X1 - WELL_W              # well inner x range WELL_X0..IN_X1
PART_A = (WELL_X0 - WALL, WELL_X0)    # wall between Pi bay and well
WELL_Y1 = IN_Y0 + WELL_D
PART_B = (WELL_Y1, WELL_Y1 + WALL)    # wall behind the well
BANK_Y1 = IN_Y1                       # bank stands against the back wall
BANK_Y0 = BANK_Y1 - BANK_T - 0.6
BANK_X0 = IN_X1 - 17 - BANK_L         # leaves a plug gap at its left end
DRUM_C = ((WELL_X0 + IN_X1) / 2 + 4, IN_Y0 + WELL_D / 2 + 3)
KEY_C = (IN_Y0 + 16, 62.0)            # keystone centre (y, z) in wall A, near the front
MAST_C = (KB_CX, TY0 + 30)            # mast centre on the lid
SLED_W, SLED_D = 70.0, 72.0
SLED_C = (min(KB_CX - 18, PART_A[0] - SLED_W / 2 - 22), IN_Y0 + 4 + SLED_D / 2)
SLED_HOLES = [(SLED_C[0] + dx, SLED_C[1] + dy) for dx in (-30, 30) for dy in (-31, 31)]
CORNERS = [(IN_X0 + 4.5, IN_Y0 + 4.5), (IN_X1 - 4.5, IN_Y0 + 4.5),
           (IN_X0 + 4.5, IN_Y1 - 4.5), (IN_X1 - 4.5, IN_Y1 - 4.5)]
DECK_SCREWS = [(30.0, 17.0), (90.0, 17.0), (160.0, 17.0)]   # (x, z) on the deck/tower joint
HUB_CABLE_X = (96.0, 108.0)            # where the hub cable passes into the tower
MAST_HOLES = [(MAST_C[0] + dx, MAST_C[1] + dy) for dx in (-36, 36) for dy in (-13, 13)]
CABLE_HOLE = (MAST_C[0] - 20, MAST_C[1] + 23, MAST_C[0] + 20, MAST_C[1] + 41)  # HDMI plug fits through

def on_slope(m):
    """Place something drawn with the deck's top surface as z = 0."""
    return m.rotate((SLOPE, 0, 0)).translate((0, 0, DECK_FRONT_H))

def deck_top(y):
    return DECK_FRONT_H + y * math.tan(math.radians(SLOPE))

# ---------------------------------------------------------------- deck
HUB_X1 = W - 2.0                       # hub port face sits on a 2 mm right wall
HUB_X0 = HUB_X1 - HUB_W - 1.0
HUB_Y0 = 15.0
HUB_Y1 = HUB_Y0 + HUB_L + 2.0
COVER_T = 2.0
HUB_Z0 = COVER_T                       # hub rests on its cover plate
HUB_REC = (HUB_X0 - 4, HUB_Y0 - 4, HUB_X1, HUB_Y1 + 4)
HUB_COVER_SCREWS = [(HUB_X0 + 3, HUB_Y0 - 2), (HUB_X1 - 5, HUB_Y0 - 2),
                    (HUB_X0 + 3, HUB_Y1 + 2), (HUB_X1 - 5, HUB_Y1 + 2)]

def make_deck():
    body = rbox(0, 0, 0, W, DECK_D, DECK_BACK_H + 5, (8, 8, 0, 0))
    body -= on_slope(box(-5, -60, 0, W + 5, DECK_D + 60, 80))
    # soften the wrist edge
    body -= M.cube((W + 10, 8, 8), center=True).rotate((45, 0, 0)).translate((W / 2, 0, DECK_FRONT_H))
    cuts = []
    # keyboard pocket, tilted with the deck
    pw, pd = KB_W + 2 * KB_CLR, KB_D + 2 * KB_CLR
    cuts.append(on_slope(box(KB_X0, KB_Y0, -KB_SINK, KB_X0 + pw, KB_Y0 + pd, 40)))
    # finger scoops left and right to lift the keyboard out
    ymid = KB_Y0 + pd / 2
    cuts.append(on_slope(cyl(KB_X0, ymid, -KB_SINK, 40, 24)))
    cuts.append(on_slope(cyl(KB_X0 + pw, ymid, -KB_SINK, 40, 18)))
    # hub bay, open underneath, closed by a screwed cover plate
    cuts.append(box(HUB_X0, HUB_Y0, -1, HUB_X1, HUB_Y1, HUB_Z0 + HUB_H + 1))
    cuts.append(box(*HUB_REC[:2], -1, *HUB_REC[2:], COVER_T))
    # port window through the right side
    pz0, pz1 = HUB_Z0 + 1.0, HUB_Z0 + HUB_H - 1.0
    py0 = (HUB_Y0 + HUB_Y1) / 2 - HUB_PORTS_L / 2
    cuts.append(box(HUB_X1 - 1, py0, pz0, W + 1, py0 + HUB_PORTS_L, pz1))
    for x, y in HUB_COVER_SCREWS:
        cuts.append(cyl(x, y, COVER_T - 1, COVER_T + 7, M3_PILOT))
    # hub cable groove underneath, out through the back face into the tower
    g = 10.0
    cuts.append(box(HUB_X0 + 8, HUB_Y1 - 2, -1, HUB_X0 + 8 + g, DECK_D - 3, g))
    cuts.append(box(HUB_CABLE_X[0], DECK_D - 3 - g, -1, HUB_X0 + 8 + g, DECK_D - 3, g))
    cuts.append(box(HUB_CABLE_X[0], DECK_D - 3 - g, -1, HUB_CABLE_X[0] + g, DECK_D + 1, g))
    # screws from inside the tower into the deck's back face
    for x, z in DECK_SCREWS:
        cuts.append(cyl_y(DECK_D - 14, DECK_D + 1, x, z, M3_PILOT))
    # rubber feet
    for x, y in [(14, 14), (KB_X0 + pw - 10, 12), (14, DECK_D - 14), (KB_X0 + pw - 10, DECK_D - 16)]:
        cuts.append(cyl(x, y, -1, 1.0, 12.5))
    return body - union(cuts)

def make_hub_cover():
    x0, y0, x1, y1 = HUB_REC
    p = box(x0 + 0.25, y0 + 0.25, 0, x1 - 0.25, y1 - 0.25, COVER_T)
    for x, y in HUB_COVER_SCREWS:
        p -= cyl(x, y, -1, COVER_T + 1, M3_CLEAR)
        p -= M.cylinder(1.6, M3_HEAD / 2, M3_CLEAR / 2, circular_segments=SEG).translate((x, y, -0.01))
    return p

# ---------------------------------------------------------------- tower
def make_tower():
    outer = rbox(0, TY0, 0, W, TY1, TOWER_H, (0, 0, 8, 8))
    t = outer - box(IN_X0, IN_Y0, WALL, IN_X1, IN_Y1, TOWER_H + 1)
    adds = []
    # cable well walls
    adds.append(box(PART_A[0], IN_Y0 - 0.1, 0, PART_A[1], PART_B[1], TOWER_H))
    adds.append(box(PART_A[0], PART_B[0], 0, IN_X1 + 0.1, PART_B[1], TOWER_H))
    # cable drum: a hollow post with a rounded top
    dx, dy = DRUM_C
    adds.append(cyl(dx, dy, 0, TOWER_H - 6, DRUM_D) - cyl(dx, dy, WALL, TOWER_H, DRUM_D - 6))
    adds.append(M.sphere(DRUM_D / 2, 64).scale((1, 1, 0.25)).translate((dx, dy, TOWER_H - 6)))
    # corner screw columns for the lid
    for x, y in CORNERS:
        adds.append(box(x - 4.5, y - 4.5, 0, x + 4.5, y + 4.5, TOWER_H))
    # bosses for the Pi sled
    for x, y in SLED_HOLES:
        adds.append(cyl(x, y, 0, WALL + 4, 8))
    # power bank: a rib keeps it standing against the back wall
    adds.append(box(BANK_X0, BANK_Y0 - 2.5, 0, BANK_X0 + BANK_L, BANK_Y0, WALL + 10))
    adds.append(box(BANK_X0 + BANK_L + 0.5, BANK_Y0 - 2.5, 0, BANK_X0 + BANK_L + 3, IN_Y1, WALL + 25))
    t = union([t] + adds)

    cuts = []
    # keystone cut-out in wall A, thinned to a 2 mm panel on the Pi side
    ky, kz = KEY_C
    cuts.append(box(PART_A[0] - 1, ky - KEY_W / 2, kz - KEY_H / 2, PART_A[1] + 1, ky + KEY_W / 2, kz + KEY_H / 2))
    cuts.append(box(PART_A[0] - 1, ky - 12, kz - 13, PART_A[1] - KEY_PANEL, ky + 12, kz + 13))
    # cable exit slot through the right wall at the top of the well
    sy = DRUM_C[1]
    cuts.append(box(IN_X1 - 1, sy - 7, TOWER_H - 14, W + 1, sy + 7, TOWER_H + 1))
    cuts.append(cyl_x(IN_X1 - 1, W + 1, sy, TOWER_H - 14, 14))
    # screws into the deck
    for x, z in DECK_SCREWS:
        cuts.append(cyl_y(TY0 - 1, TY0 + WALL + 1, x, z, M3_CLEAR))
    # hub cable from the deck
    cuts.append(box(HUB_CABLE_X[0], TY0 - 1, WALL, HUB_CABLE_X[1] + 2, TY0 + WALL + 1, WALL + 13))
    # charging window at the bank's plug end (left wall, back)
    cuts.append(box(-1, BANK_Y0 - 3, 12, WALL + 1, IN_Y1 - 1, 62))
    # vents on the left side of the Pi bay
    for i in range(7):
        y = IN_Y0 + 18 + i * 8
        cuts.append(box(-1, y, 30, WALL + 1, y + 3.5, 70))
    # lid screw pilots, sled pilots
    for x, y in CORNERS:
        cuts.append(cyl(x, y, TOWER_H - 16, TOWER_H + 1, M3_PILOT))
    for x, y in SLED_HOLES:
        cuts.append(cyl(x, y, 1.0, WALL + 5, M3_PILOT))
    # rubber feet
    for x, y in [(14, TY0 + 14), (W - 14, TY0 + 14), (14, TY1 - 14), (W - 14, TY1 - 14)]:
        cuts.append(cyl(x, y, -1, 1.0, 12.5))
    return t - union(cuts)

# ---------------------------------------------------------------- lids
WELL_LID = (PART_A[0] + 1.5, TY0, W, PART_B[0] + 1.5)   # region the well lid covers

def make_lid():
    z0 = TOWER_H
    lid = rbox(0, TY0, z0, W, TY1, z0 + LID_T, (0, 0, 8, 8))
    lid -= box(WELL_LID[0] - 0.3, TY0 - 1, z0 - 1, W + 1, WELL_LID[3] + 0.3, z0 + LID_T + 1)
    # locating lip that drops inside the outer walls
    lip = box(IN_X0 + 0.4, IN_Y0 + 0.4, z0 - 4, IN_X1 - 0.4, IN_Y1 - 0.4, z0) - \
          box(IN_X0 + 2.0, IN_Y0 + 2.0, z0 - 5, IN_X1 - 2.0, IN_Y1 - 2.0, z0 + 1)
    lip -= box(PART_A[0] - 0.4, TY0 - 1, z0 - 5, W + 1, PART_B[1] + 0.4, z0 + 1)
    for x, y in CORNERS:
        lip -= box(x - 5, y - 5, z0 - 5, x + 5, y + 5, z0 + 1)
    lip -= box(BANK_X0 + BANK_L, BANK_Y0 - 3, z0 - 5, BANK_X0 + BANK_L + 4, IN_Y1 + 1, z0 + 1)
    lid += lip
    cuts = []
    for x, y in CORNERS[:1] + CORNERS[2:]:
        cuts.append(countersink(x, y, z0 + LID_T))
    for x, y in MAST_HOLES:
        cuts.append(cyl(x, y, z0 - 1, z0 + LID_T + 1, M3_CLEAR))
    x0, y0, x1, y1 = CABLE_HOLE
    cuts.append(rbox(x0, y0, z0 - 6, x1, y1, z0 + LID_T + 1, 5))
    for i in range(8):     # vents over the Pi
        x = SLED_C[0] - 30 + i * 8
        cuts.append(rbox(x, IN_Y1 - 32, z0 - 1, x + 3.5, IN_Y1 - 26, z0 + LID_T + 1, 1.7))
    return lid - union(cuts)

def make_well_lid():
    z0 = TOWER_H
    x0, y0, x1, y1 = WELL_LID
    lid = rbox(x0, y0, z0, x1, y1, z0 + LID_T, (0, 0, 0, 0)) - box(x1 - 9, y0 - 1, z0 - 1, x1 + 1, y0 + 9, z0 + 5) \
          + rbox(x1 - 9, y0, z0, x1, y0 + 9, z0 + LID_T, (0, 8, 0, 0))
    plug = box(WELL_X0 + 0.4, IN_Y0 + 0.4, z0 - 5, IN_X1 - 0.4, WELL_Y1 - 0.4, z0) - \
           box(WELL_X0 + 2.0, IN_Y0 + 2.0, z0 - 6, IN_X1 - 2.0, WELL_Y1 - 2.0, z0 + 1)
    cx, cy = CORNERS[1]
    plug -= box(cx - 5, cy - 5, z0 - 6, cx + 5, cy + 5, z0 + 1)
    lid += plug
    lid -= countersink(cx, cy, z0 + LID_T)          # optional screw into the corner column
    # finger hole
    lid -= cyl((x0 + x1) / 2 - 8, y1 - 14, z0 - 6, z0 + LID_T + 1, 18)
    return lid

# ---------------------------------------------------------------- Pi sled
def make_sled():
    s = rbox(-SLED_W / 2, -SLED_D / 2, 0, SLED_W / 2, SLED_D / 2, 3, 5)
    adds, cuts = [], []
    px0, py0 = -PI_W / 2, -PI_D / 2
    for hx, hy in PI_HOLES:
        adds.append(cyl(px0 + hx, py0 + hy, 0, 3 + 5, 6))
        cuts.append(cyl(px0 + hx, py0 + hy, 1, 3 + 6, PI_SCREW_PILOT))
    for x, y in SLED_HOLES:
        cuts.append(countersink(x - SLED_C[0], y - SLED_C[1], 3))
    for sx in (-1, 1):                  # zip-tie slots for cables
        cuts.append(box(sx * 31 - 2, -12, -1, sx * 31 + 2, -6, 4))
        cuts.append(box(sx * 31 - 2, 6, -1, sx * 31 + 2, 12, 4))
    cuts.append(rbox(-16, -14, -1, 16, 14, 4, 4))   # save plastic, airflow under the board
    return union([s] + adds) - union(cuts)

# ---------------------------------------------------------------- screen pod (local frame)
# face on z = 0, depth +z, x across, y up. Pivot axis along x.
PIVOT = (POD_H / 2, POD_D / 2 + 1)   # (y, z) of the pivot in the pod frame
LCD_X0 = (POD_W - LCD_W) / 2 - 0.3
LCD_Y0 = (POD_H - LCD_H) / 2 - 0.3
FRONT_T = 2.5
POD_BOSSES = [(6.5, 6.5), (POD_W - 6.5, 6.5), (6.5, POD_H - 6.5), (POD_W - 6.5, POD_H - 6.5)]

def make_bezel():
    b = rbox(0, 0, 0, POD_W, POD_H, POD_D, 6)
    b -= box(WALL, WALL, FRONT_T, POD_W - WALL, POD_H - WALL, POD_D + 1)
    adds = []
    for x, y in POD_BOSSES:
        adds.append(box(x - 3.5, y - 3.5, 0, x + 3.5, y + 3.5, POD_D))
    py, pz = PIVOT
    for x0 in (0, POD_W - WALL - 7):
        adds.append(box(x0, py - 8, 0, x0 + WALL + 7, py + 8, POD_D))
    # L-shaped guides that hold the LCD module in place
    w, h = LCD_W + 0.6, LCD_H + 0.6
    for gx, gy, sx, sy in [(LCD_X0, LCD_Y0, 1, 1), (LCD_X0 + w, LCD_Y0, -1, 1),
                           (LCD_X0, LCD_Y0 + h, 1, -1), (LCD_X0 + w, LCD_Y0 + h, -1, -1)]:
        adds.append(box(min(gx - sx * 1.6, gx + sx * 10), min(gy, gy - sy * 1.6), FRONT_T,
                        max(gx - sx * 1.6, gx + sx * 10), max(gy, gy - sy * 1.6), FRONT_T + LCD_PANEL_T - 0.3))
        adds.append(box(min(gx, gx - sx * 1.6), min(gy - sy * 1.6, gy + sy * 10), FRONT_T,
                        max(gx, gx - sx * 1.6), max(gy - sy * 1.6, gy + sy * 10), FRONT_T + LCD_PANEL_T - 0.3))
    b = union([b] + adds)
    cuts = []
    vx = POD_W / 2 + VIEW_DX; vy = POD_H / 2 + VIEW_DY
    # window, chamfered toward the viewer
    cuts.append(M.batch_hull([box(vx - VIEW_W / 2, vy - VIEW_H / 2, FRONT_T - 0.01, vx + VIEW_W / 2, vy + VIEW_H / 2, FRONT_T + 1),
                              box(vx - VIEW_W / 2 - 2, vy - VIEW_H / 2 - 2, -1, vx + VIEW_W / 2 + 2, vy + VIEW_H / 2 + 2, -0.99)]))
    for x, y in POD_BOSSES:
        cuts.append(cyl(x, y, 6, POD_D + 1, M3_PILOT))
    for x in (-1, POD_W - WALL - 9):
        cuts.append(cyl_x(x, x + WALL + 10, py, pz, M4_PILOT))
    # cable exit through the bottom edge (HDMI + power), behind the LCD
    cuts.append(box(POD_W / 2 - 35, -1, FRONT_T + LCD_T, POD_W / 2 + 35, WALL + 1, POD_D + 1))
    # micro-USB power plug through the left wall
    uy = LCD_Y0 + 0.3 + USB_Y
    cuts.append(box(-1, uy - 7, FRONT_T + LCD_PANEL_T, WALL + 1, uy + 7, POD_D + 1))
    return b - union(cuts)

def pod_presses():
    """What the back cover pushes on: two rails on the PCB's bare top/bottom edges
    (clear of the HDMI socket and the pin header) and a pad on the glass past the PCB's end."""
    px, py = LCD_X0 + 0.3, LCD_Y0 + 0.3
    pcb_y0, pcb_y1 = py + (LCD_H - LCD_PCB_H) / 2, py + (LCD_H + LCD_PCB_H) / 2
    zp = FRONT_T + LCD_T + 0.2
    return [box(px + 46, pcb_y0 + 0.3, zp, px + 72, pcb_y0 + 3.0, POD_D + 0.1),
            box(px + 46, pcb_y1 - 3.0, zp, px + 72, pcb_y1 - 0.3, POD_D + 0.1),
            box(px + LCD_PCB_L + 2, py + 18, FRONT_T + LCD_PANEL_T + 0.2, px + LCD_W - 1.5, py + LCD_H - 18, POD_D + 0.1)]

def make_pod_back():
    """Back cover, in the pod frame (sits on z = POD_D)."""
    z0 = POD_D
    c = rbox(0, 0, z0, POD_W, POD_H, z0 + POD_COVER_T, 6)
    adds = pod_presses()
    c = union([c] + adds)
    cuts = [countersink(x, y, z0 + POD_COVER_T) for x, y in POD_BOSSES]
    cuts.append(box(POD_W / 2 - 35, -1, z0 - 1, POD_W / 2 + 35, 8, z0 + POD_COVER_T + 1))
    for i in range(6):
        x = POD_W / 2 - 22 + i * 8.8
        cuts.append(rbox(x, POD_H - 30, z0 - 1, x + 3.5, POD_H - 14, z0 + POD_COVER_T + 1, 1.7))
    return c - union(cuts)

# ---------------------------------------------------------------- mast (yoke)
ARM_T = 6.0
def make_mast():
    cx, cy = MAST_C
    gap = POD_W / 2 + 0.5
    base = rbox(cx - gap - ARM_T - 2, cy - 20, 0, cx + gap + ARM_T + 2, cy + 20, 4, 5)
    parts = [base]
    for s in (-1, 1):
        x0 = cx + s * gap if s > 0 else cx - gap - ARM_T
        arm = box(x0, cy - 9, 0, x0 + ARM_T, cy + 9, MAST_H) + cyl_x(x0, x0 + ARM_T, cy, MAST_H, 18, 64)
        gus = M.batch_hull([box(x0, cy - 18, 0, x0 + ARM_T, cy + 18, 4), box(x0, cy - 9, 0, x0 + ARM_T, cy + 9, 40)])
        parts += [arm, gus]
    m = union(parts)
    cuts = [cyl(x, y, -1, 5, M3_CLEAR) for x, y in MAST_HOLES]
    cuts += [cyl(x, y, 3, 30, 7) for x, y in MAST_HOLES]   # room for the screw heads
    cuts.append(cyl_x(cx - gap - ARM_T - 1, cx + gap + ARM_T + 1, cy, MAST_H, M4_CLEAR))
    cuts.append(rbox(cx - 30, cy - 12, -1, cx + 30, cy + 12, 5, 5))   # lighter base
    return m - union(cuts)

# ---------------------------------------------------------------- placing the pod in the world
def pod_to_world(m, tilt=SCREEN_TILT):
    py, pz = PIVOT
    m = m.translate((-POD_W / 2, -py, -pz))         # pivot at origin
    m = m.rotate((90, 0, 0))                        # (x, y, z) -> (x, -z, y)
    m = m.mirror((0, 1, 0))                         # -> (x, z, y): depth goes back (+y), face toward you
    m = m.rotate((-tilt, 0, 0))                     # lean the top back
    return m.translate((MAST_C[0], MAST_C[1], TOWER_H + LID_T + MAST_H))

# ---------------------------------------------------------------- stand-ins for the real parts
def dummies():
    pw, pd = KB_W + 2 * KB_CLR, KB_D + 2 * KB_CLR
    kb = on_slope(box(KB_X0 + KB_CLR, KB_Y0 + KB_CLR, -KB_SINK + 0.05, KB_X0 + KB_CLR + KB_W, KB_Y0 + KB_CLR + KB_D, -KB_SINK + KB_T))
    hub = box(HUB_X0 + 0.5, HUB_Y0 + 1, HUB_Z0 + 0.05, HUB_X0 + 0.5 + HUB_W, HUB_Y0 + 1 + HUB_L, HUB_Z0 + HUB_H)
    bank = box(BANK_X0 + 0.2, BANK_Y0 + 0.2, WALL + 0.05, BANK_X0 + BANK_L, BANK_Y1 - 0.1, WALL + BANK_H)
    sz = WALL + 4 + 3 + 5
    pi = box(SLED_C[0] - PI_W / 2, SLED_C[1] - PI_D / 2, sz + 0.05, SLED_C[0] + PI_W / 2, SLED_C[1] + PI_D / 2, sz + 1.6) + \
         box(SLED_C[0] - PI_W / 2, SLED_C[1] - PI_D / 2, sz + 1.6, SLED_C[0] - PI_W / 2 + 16, SLED_C[1] - PI_D / 2 + 21, sz + 15)
    px, py = LCD_X0 + 0.3, LCD_Y0 + 0.3
    zb = FRONT_T + LCD_T                                   # back of the PCB
    pcb_y0 = py + (LCD_H - LCD_PCB_H) / 2
    lcd = union([
        box(px, py, FRONT_T + 0.05, px + LCD_W, py + LCD_H, FRONT_T + LCD_PANEL_T),             # glass
        box(px, pcb_y0, FRONT_T + LCD_PANEL_T, px + LCD_PCB_L, pcb_y0 + LCD_PCB_H, zb),          # PCB
        box(px + 9, pcb_y0 + LCD_PCB_H - 6.5, zb, px + 42, pcb_y0 + LCD_PCB_H - 1.4, FRONT_T + LCD_DEPTH),  # header
        box(px + HDMI_X[0], pcb_y0 - 0.5, zb, px + HDMI_X[1], pcb_y0 + 11.5, zb + 4.5),       # HDMI
        box(px - 1.0, py + USB_Y - 4, zb, px + 5.5, py + USB_Y + 4, zb + 3),                   # micro-USB
    ])
    lcd = pod_to_world(lcd)
    ky, kz = KEY_C
    key = box(PART_A[0] - 14, ky - KEY_W / 2, kz - KEY_H / 2, PART_A[1] + 14, ky + KEY_W / 2, kz + KEY_H / 2)
    return {"keyboard": kb, "usb hub": hub, "power bank": bank, "orange pi": pi, "lcd": lcd, "keystone": key}

# ---------------------------------------------------------------- output
def write_stl(m, path):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3].astype(np.float32)
    t = np.asarray(mesh.tri_verts)
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    rec = np.zeros(len(t), dtype=[("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
    rec["n"] = n; rec["v"] = tri
    with open(path, "wb") as f:
        f.write(b"BoltOS Mini case".ljust(80, b" "))
        f.write(struct.pack("<I", len(t)))
        f.write(rec.tobytes())

def to_bed(m):
    (x0, y0, z0, x1, y1, z1) = m.bounding_box()
    return m.translate((-(x0 + x1) / 2, -(y0 + y1) / 2, -z0))

def build():
    tower, deck, lid, well_lid = make_tower(), make_deck(), make_lid(), make_well_lid()
    hub_cover = make_hub_cover()
    sled = make_sled()
    bezel, back, mast = make_bezel(), make_pod_back(), make_mast()
    sled_w = sled.translate((SLED_C[0], SLED_C[1], WALL + 4))
    world = {
        "deck": deck, "hub_cover": hub_cover, "tower": tower, "lid": lid, "well_lid": well_lid,
        "pi_sled": sled_w, "mast": mast.translate((0, 0, TOWER_H + LID_T)),
        "screen_bezel": pod_to_world(bezel), "screen_back": pod_to_world(back),
    }
    # print orientation for each part
    printed = {
        "deck": deck,
        "hub_cover": hub_cover,
        "tower": tower,
        "lid": lid.mirror((0, 0, 1)),          # flat top on the bed, lip up
        "well_lid": well_lid.mirror((0, 0, 1)),
        "pi_sled": sled,
        "mast": mast,
        "screen_bezel": bezel,                 # face down
        "screen_back": back.mirror((0, 0, 1)), # outside face down
    }
    return world, printed

def main():
    world, printed = build()
    out = os.path.join(HERE, "stl"); os.makedirs(out, exist_ok=True)
    print("part            size on the bed (mm)          volume cm3")
    for name, m in printed.items():
        m = to_bed(m)
        x0, y0, z0, x1, y1, z1 = m.bounding_box()
        write_stl(m, os.path.join(out, f"{name}.stl"))
        flag = "" if max(x1 - x0, y1 - y0) <= 220 else "  <-- bigger than 220 mm"
        print(f"{name:14s} {x1-x0:6.1f} x {y1-y0:6.1f} x {z1-z0:6.1f}   {m.volume()/1000:7.1f}{flag}")
    allw = union(list(world.values()))
    x0, y0, z0, x1, y1, z1 = allw.bounding_box()
    print(f"\nassembled: {x1-x0:.0f} x {y1-y0:.0f} x {z1-z0:.0f} mm "
          f"({(x1-x0)/25.4:.1f} x {(y1-y0)/25.4:.1f} x {(z1-z0)/25.4:.1f} in)")
    write_stl(allw, os.path.join(out, "_assembled_preview.stl"))
    # collisions between the printed parts and the things that go inside
    print("\nfit check (overlap of each part with the printed case, should be ~0):")
    case = union([m for k, m in world.items()])
    for name, d in dummies().items():
        ov = (d ^ case).volume()
        print(f"  {name:12s} {ov:8.1f} mm3")
    # printed parts touching each other (other than resting faces)
    names = list(world)
    print("\npart-vs-part overlaps (mm3):")
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            ov = (world[names[i]] ^ world[names[j]]).volume()
            if ov > 1:
                print(f"  {names[i]} x {names[j]}: {ov:.1f}")
    if "--no-preview" not in sys.argv:
        import preview
        preview.render(world, dummies(), os.path.join(HERE, "preview"))

if __name__ == "__main__":
    main()
