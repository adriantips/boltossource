# Game Boy

A DMG emulator for BoltOS: SM83 core, scanline PPU, four sound channels, the
divider/timer pair, and MBC1/2/3/5. About 1,500 lines.

Put `.gb` files in `/home/user/roms` and open the app. Nothing is shipped with
it -- a Game Boy without a cartridge is exactly as useful as the real thing
was.

---

## Controls

| | |
| --- | --- |
| arrow keys, or `W` `A` `S` `D` | d-pad |
| `X` | A |
| `Z` | B |
| `Enter` | Start |
| `Right Shift` or `Backspace` | Select |
| `Esc` | eject, and go back to the cartridge list |

Cartridge RAM is written back to `<rom>.sav` next to the ROM every few seconds
and whenever the window loses focus, so a save game survives being closed and
survives a hard power-off by at most four seconds.

---

## What is emulated

**CPU.** The whole SM83 instruction set, cycle-counted in T-states. Sharp's
chip, not a Z80 and not an 8080: no IX/IY, no shadow registers, no parity flag,
and a handful of instructions (`LDH`, `LD (HL+)`, `ADD SP,n`) belonging to
neither. The HALT bug is implemented, `EI` is delayed by one instruction, and
the low nibble of `F` is wired to zero the way the hardware wires it.

**PPU.** Background, window and sprites, drawn one scanline at a time on the
mode 3 to mode 0 edge, with the real mode timings underneath (80 T-states of
OAM search, 172 of transfer, 204 of blanking, 456 to the line). Ten sprites
per line, priority by X then by OAM index, 8x16 tiles, both flips, and the
background-priority attribute tested against background colour 0 rather than
against the background as a whole. The window keeps its own line counter, which
only advances on lines where it was visible -- getting that wrong tears every
status bar in the library.

**APU.** Two squares with envelopes and a sweep, the wavetable channel, and
the noise LFSR including its narrow mode. The frame sequencer runs at 512 Hz
off the same T-state count as everything else, which is the only way the music
stays in time with the game. Output is resampled to 48 kHz by counting
T-states, so it is nearest-neighbour and it aliases; on square waves that is
almost inaudible.

**Cartridges.** MBC1 (including the advanced banking mode the multi-carts
use), MBC2 with its four-bit on-mapper RAM, MBC3 and MBC5. Battery RAM is
saved; anything else is refused by name rather than run as if it were an MBC1,
because that produces a black screen and no clue.

## What is not

No Game Boy Color: a `.gbc` loads and runs in DMG mode if the cartridge
supports it, and looks wrong otherwise. No link cable. No MBC3 real-time
clock -- latching it does nothing and reading it gives zeroes, so a game that
shows a time shows midnight and a game that gates on elapsed time never
advances. No pixel FIFO, so the handful of demos that perturb the shifter
mid-line will not look right; games do not do this.

OAM DMA is instant rather than taking its 640 T-states. No game has ever been
able to see the difference, because they all spin in HRAM for exactly that
long afterwards.

---

## The files

| | |
| --- | --- |
| `gb.h` | the machine's state, in one struct |
| `gb_cpu.c` | the SM83 core |
| `gb_mem.c` | the bus, the cartridge, the mappers, the timer, the joypad |
| `gb_ppu.c` | the scanline renderer |
| `gb_apu.c` | four channels and the frame sequencer |
| `../kernel/app_gameboy.c` | the window, the ROM picker and the save files |

Nothing under `gb/` has heard of BoltOS. It is handed a ROM image and a button
bitmap and produces a framebuffer and audio samples.

---

## Testing it

```
python test/t_gameboy.py --host-only
```

Where there is an independent oracle, the tests use one rather than asserting
that the code does what the code does:

- **DAA** is re-derived in the test straight from the hardware description and
  diffed against the core over all 4,096 inputs. Two implementations from the
  same spec in different shapes; if they agree, neither is the one that was
  typed wrong. DAA is where a core like this is usually broken, and the symptom
  is a score displayed in hexadecimal three levels into a game.
- The **flag tests** assert results worked out by hand from the instruction
  set -- that `0F + 01` sets the half carry and nothing else, that `RLCA`
  clears Z where its `CB` twin does not, that `POP AF` masks the low nibble,
  that `ADD SP,n` takes its carries from the low byte.
- The **PPU tests** build a tile by hand and assert the pixels that have to
  come out of it, including sprite priority against background colour 0.
- The last one assembles a **small ROM by hand** -- build a tile, fill the tile
  map, set the palette, enable the LCD, spin -- runs it for twelve frames and
  checks the screen. If that passes, the CPU executed, its writes reached VRAM
  through the bus, the PPU read them back and the frame loop terminated, which
  is the part no unit test covers and the part that is actually broken when a
  game shows a black screen.

The QEMU half is a smoke test only. There is no ROM on the guest to run, and
getting a binary cartridge into the image from a test would mean either a
network fetch or baking one into the build.
