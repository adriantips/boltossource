# BoltStrike

A round-based defusal shooter for BoltOS. Buy a gun, walk to a bombsite, and
try to be the team that is still standing. Two sites, five bots a side, a
forty-second fuse. About 2,300 lines and not one byte of artwork.

Open it from the desktop, click the window to take the mouse, and press Escape
to give it back.

---

## Why it is not Counter-Strike

Because Counter-Strike is not free to copy, and nothing about it here is. The
map is original geometry in a familiar shape. Every wall texture, every
soldier, every gun and every sound in this directory is generated at startup
by code you can read in `st_tex.c` and `st_sfx.c`. The weapon names are made
up. There is no `.bsp`, no `.wad`, no `.wav` -- and the point is not only
legal tidiness: the whole game adds under a megabyte to the kernel, and it
does that by not having any assets to carry.

If it feels familiar anyway, that is the layout language doing the work:
two bombsites, a long corridor, a mid lane through a doorway, tunnels on the
other side. Anyone who has played a defusal map will know roughly where they
are within a round. Nobody's files were involved.

---

## Controls

| | |
| --- | --- |
| `W` `A` `S` `D` | move |
| mouse | look |
| left button | fire |
| right button | scope, on the bolt rifle |
| `shift` | walk quietly |
| `1` `2` `3` | primary / pistol / knife |
| `R` | reload |
| `E` | hold to plant, or to defuse |
| `B` | buy menu, during freeze time and in your own spawn |
| `Tab` | scoreboard |
| `Esc` | release the mouse, or close the buy menu |

Arrow keys turn while the mouse is not captured, which is how the test drives
it without taking the pointer away from anyone.

---

## The rules

Rounds are seven seconds of freeze time, then up to 115 seconds. The Ts win by
planting and letting the fuse run out, or by eliminating the CTs; the CTs win
by defusing, by eliminating the Ts, or by running the clock out. Killing
someone pays according to the gun you used, winning a round pays 3,250 and
losing pays 1,900, capped at 16,000. Armour carries between rounds and your
magazines are refilled at the start of each one.

Defusing takes 9.5 seconds, or 4.8 with a kit -- one CT in three gets one.
Planting takes 3 seconds and pays 800.

A head shot is a shot through the middle of the silhouette. There is no
vertical axis in this game, so that is the only honest way to have head shots
at all: it rewards precision instead of pretending to model a neck.

---

## The guns

| | price | notes |
| --- | --- | --- |
| Knife | -- | reaches about a metre and a half |
| GK-9 / M-45 | 200 | what you start with; the GK-9 is automatic |
| DE-50 | 700 | one shot, most of a torso |
| MP-9 | 1250 | cheap, fast, falls off badly with range |
| SG-12 | 1700 | eight pellets, and useless past a room |
| AR-47 / AR-15C | 2700 / 3100 | the T and CT rifles |
| BR-338 | 4750 | one shot, scoped, and slow to cycle |

Moving widens your spread by a lot, which is the one movement rule that
matters and the reason standing still is worth anything.

---

## How it draws

One DDA per screen column, not per pixel. Every wall in the game is the same
height, so a column is a single texture strip and the scene costs 320 rays
rather than 64,000 -- which is the difference between a game and a slideshow
on a machine with no acceleration. Floors are cast afterwards, one row at a
time, at a constant world distance, so the bombsite markings are really on the
ground.

Sprites are billboards, depth-tested per column against the wall pass. Each
soldier has five drawn views mirrored into eight facings, two walk frames and
a body, built out of a dozen rounded boxes each.

The frame is 320x200 and the window scales it. The HUD is drawn afterwards at
the window's own resolution, so the numbers stay sharp however coarse the 3D
underneath is.

---

## The bots

Four states -- roam, fight, hunt, and the objective -- over a breadth-first
flow field. 2,304 cells is nothing to flood, and a bot that reads its next
step out of an array cannot get stuck replanning or walk into a wall it was
told about two seconds ago.

They see in a cone, not a sphere: a bot that shoots you from behind its own
head is the fastest way to make a game feel cheap. Skill sets the turn rate
and the residual aim error, and nothing else.

---

## The files

| | |
| --- | --- |
| `strike.h` | the shapes everything else shares |
| `st_map.c` | the map as 48 rows of text, plus line of sight and navigation |
| `st_tex.c` | every pixel in the game |
| `st_render.c` | the raycaster, the sprites and the gun in your hands |
| `st_game.c` | rounds, money, damage, and the bots |
| `st_sfx.c` | sixteen synthesised sounds and an eight-voice mixer |
| `../kernel/app_strike.c` | the window, the input and the HUD |

The map is not typed by hand. It comes out of a generator that flood-fills
from CT spawn and refuses to emit a layout with a stranded floor cell; what is
in `st_map.c` is that generator's checked output.

---

## Testing it

```
python test/t_strike.py --host-only
```

compiles `st_map.c` and `st_game.c` on the host with nothing under them but
`malloc`, and plays several hundred rounds of bots against each other in about
a second. It asserts the things no human will sit and watch for: that every
round ends by itself, that no bot ever finishes a tick inside a wall, that
both teams win sometimes, that line of sight is symmetric in both directions,
and that the bomb can be planted, can be defused, and goes off when it is not.

Without `--host-only` it also boots a real machine and opens the app.

---

## What it does not do

No multiplayer -- the socket layer is there and two BoltOS machines on one
wire is a tested arrangement elsewhere in this tree, but none of it is wired
up here. No grenades, no crouching, no ladders, no vertical aim beyond a
cosmetic pitch. The bots do not hear gunfire, only see it; `noise_ms` is
plumbed through and never set by anything but sight, which is a loose end
rather than a design.
