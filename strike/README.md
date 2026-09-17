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
| walk over it | pick a dropped bomb back up |
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

Defusing takes 9.5 seconds, or 4.8 with a kit -- one CT in three gets one, and
it takes that long however many people are standing on it. Planting takes 3
seconds and pays 800.

**Halves.** Six rounds a side, then everybody changes ends: money back to 800,
armour and guns gone, kills kept, the two side scores untouched. You start as a
CT because defending is the friendlier side to learn a map from -- the map comes
to you -- and you are a T from round seven, which is when the plant key starts
meaning something. Before this existed you were a CT for ever, which made half
of a game about planting a bomb something you watched other people do.

**The bomb on the floor.** Kill the carrier and the bomb drops where it fell;
any T who walks over it picks it up, and it sits on both minimaps in amber until
somebody does. A CT cannot pick it up and cannot defuse it -- it was never
planted. This is the first thing a CT tries every round, and it used to remove
the objective from the round entirely and silently: the drop was written, the
pickup was not, and nothing was drawn.

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
a body, built out of a dozen rounded boxes each. The mirror happens while the
sprite is being drawn, not before: the whole list is gathered and sorted first,
so a mirrored view baked into one shared scratch buffer at gather time meant
every mirrored soldier in the frame wore the last one's pose. Effects went the
same way and were all the colour of whichever one was gathered last.

The wall pass records what it covered, per column, and the sky and floor passes
skip those pixels. The floor used to be drawn under the walls and thrown away,
which cost a third of the frame in a bombsite -- where the walls are close, they
cover everything, and standing still next to the bomb is exactly what the game
asks you to do for three seconds.

Getting shot flashes the frame red for 450 ms. It used to key off health alone,
so one graze early in a round left the screen tinted, and repainted 64,000
pixels every frame for the rest of it.

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

The objective outranks the other three. A bot standing on the site with the
bomb plants it unless there is somebody in sight right now -- which sounds
obvious and was not what happened: planting lived at the bottom of the roam
case, and seeing anybody at all sets a four-second hunt timer, so a carrier that
caught one glimpse of a CT on the way in spent the rest of the round hunting.
Over four hundred rounds, forty per cent ended on "Time expired" with the bomb
still in somebody's hands. It is zero now.

The map is CT-sided -- they are already there, and whoever is looking at the
doorway first wins the fight -- and the bots win about a third of rounds as Ts.
That is a property of the layout rather than a bug, but it is why
`test/host/strike.c` plays fifty rounds before asserting that both sides win
sometimes, and not eight.

## What it costs

The desktop loop wakes on every PIT tick and the PIT runs at 1000 Hz, so
`strike_pump` is called a thousand times a second. Only the draw was throttled.
Everything else -- nine bots thinking, the input, the mixer, the mouse recentre
-- ran on every one of those ticks, and each roaming bot was scanning all 2,304
cells of the map twice to find out where the middle of a bombsite was. That is
about thirty million cell visits a second, on a machine with no acceleration,
to answer a question whose answer is in the map file.

The centres are worked out once in `st_map_load` now, and the simulation runs on
a 16 ms step instead of a 1 ms one. Measured on the host, a live round's
`st_step` went from 19 to 1.3 microseconds; multiplied by sixteen times fewer
calls, that is most of what the game was spending.

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
cosmetic pitch. You cannot pick up a dead man's rifle; the bomb is the only
thing in the game that changes hands. The bots do not hear gunfire, only see
it; `noise_ms` is plumbed through and never set by anything but sight, which is
a loose end rather than a design.

Bodies lie there for nine seconds and fade out over the last one. They used to
stay for the whole round, which was an `if` with an empty body rather than a
decision, and by the time the bomb went down there were nine of them in the
draw list every frame -- all facing the wrong way, because the mirror was
skipped for the dead.
