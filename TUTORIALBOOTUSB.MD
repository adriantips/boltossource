# Booting BoltOS from a USB stick

This is the whole story of getting BoltOS off your build machine and onto real
hardware: which image to write, how to write it, what to change in firmware
setup, and — the part most guides skip — how to read the screen when it does not
work.

It assumes you can run `./build.sh` and that you have a USB stick you are
willing to erase completely.

- [The short version](#the-short-version)
- [Which image](#which-image)
- [Writing the stick](#writing-the-stick)
- [Firmware setup](#firmware-setup)
- [What a good boot looks like](#what-a-good-boot-looks-like)
- [When it does not boot](#when-it-does-not-boot)
- [Installing to the internal disk](#installing-to-the-internal-disk)
- [How the boot chain actually works](#how-the-boot-chain-actually-works)

---

## The short version

```bash
./build.sh
```

Write `iso/boltusb.img` to the stick with Rufus (**DD mode**), `dd`, or
balenaEtcher. Turn Secure Boot off. Boot the machine from it.

If the machine offers you the stick twice — once plain and once with "UEFI:" in
front of the name — the plain one is faster to the desktop and the UEFI one is
more reliable. Try plain first; switch to UEFI the moment it misbehaves, and
give that one up to three minutes before deciding it has hung.

Everything below is detail, and the troubleshooting section is the part worth
reading before you start rather than after.

---

## Which image

`./build.sh` produces three bootable things, and they are not interchangeable.

| File | Write it to | Boots |
| --- | --- | --- |
| `iso/boltusb.img` | a USB stick | **legacy BIOS and UEFI** |
| `iso/boltos.iso` | a CD/DVD, or a VM's optical drive | legacy BIOS |
| `iso/os.img` | nothing physical — it is for QEMU | legacy BIOS only |

**Use `iso/boltusb.img` for a USB stick.** It is about 106 MiB. The repo only
carries `iso/boltusb.img.gz` (10 MiB, since GitHub refuses files over 100 MB),
so a download from GitHub gets that one: Rufus and balenaEtcher write the `.gz`
as it is, and for `dd` either `gunzip` it first or pipe `gunzip -c` into it.

`os.img` is the one to avoid here, and it is the easy mistake to make because it
is the image `run.sh` uses and the one the rest of the docs used to name. It is
deliberately minimal: no partition table at all, just a boot sector, stage2, a
header and a raw kernel. QEMU does not care. Real firmware does — it decides how
to present a USB device by reading its MBR, and a partition table of 64 zero
bytes gets treated differently by every vendor. Some emulate the stick as a
floppy, some refuse it outright, and the ones that do boot it hand INT 13h a
disk geometry they made up. On top of that `os.img` has no EFI system partition,
so a machine with no CSM cannot boot it at all.

`boltusb.img` is laid out the way a real disk is laid out:

```
LBA 0          stage1, and a genuine MBR partition table
LBA 1..32      stage2
LBA 33         BoltOS boot header  (kernel location, size, checksum)
LBA 34..       kernel.bin, raw, in the gap before the first partition
1 MiB-aligned  partition 1: FAT32 EFI system partition, active
                 /EFI/BOOT/BOOTX64.EFI     <- what UEFI firmware runs
                 /kernel.bin               <- what BOOTX64.EFI then loads
1 MiB-aligned  partition 2: empty BoltFS partition
```

The kernel is on the stick twice, which is deliberate rather than sloppy. UEFI
firmware can only read files out of a FAT filesystem, and stage2 — running in
real mode with nothing but INT 13h — cannot read FAT at all. Each path gets a
copy it can actually reach.

---

## Writing the stick

Every method below **erases the entire stick**. Not a partition on it. All of
it. Check the device name twice; on Windows, close every Explorer window showing
the stick first.

### Windows — Rufus

1. Open Rufus, pick the stick under **Device**.
2. **SELECT** → `iso\boltusb.img`.
3. Rufus may ask whether to write in **ISO Image mode** or **DD Image mode**.
   Choose **DD Image mode**. If it does not ask, it has already chosen DD, which
   is correct for a `.img`.
4. Partition scheme **MBR**, target system **BIOS or UEFI**.
5. **START**, and let it finish before pulling the stick.

Rufus normally recognises a raw disk image and picks DD mode on its own. The one
thing to avoid is letting it "fix" the image or add its own bootloader.

### Windows — PowerShell, no extra tools

```powershell
$img  = "C:\path\to\boltOS\iso\boltusb.img"
$disk = 2                     # CHECK THIS with Get-Disk first
Get-Disk
```

Confirm from `Get-Disk` that number 2 really is the stick and really is the size
you expect, then:

```powershell
Clear-Disk -Number $disk -RemoveData -RemoveOEM -Confirm:$false
$src = [System.IO.File]::OpenRead($img)
$dst = New-Object System.IO.FileStream "\\.\PhysicalDrive$disk", 'Open', 'Write'
$src.CopyTo($dst, 1MB)
$dst.Flush(); $dst.Close(); $src.Close()
```

### Linux

```bash
lsblk -o NAME,SIZE,MODEL,TRAN          # find the stick; TRAN says "usb"
sudo dd if=iso/boltusb.img of=/dev/sdX bs=4M conv=fsync status=progress
sudo sync
```

`of=/dev/sdX`, the whole device — not `/dev/sdX1`.

### macOS

```bash
diskutil list                          # find the disk, e.g. /dev/disk4
diskutil unmountDisk /dev/disk4
sudo dd if=iso/boltusb.img of=/dev/rdisk4 bs=4m
diskutil eject /dev/disk4
```

`rdisk4` rather than `disk4` is a large speed difference on macOS.

### Checking the write

Worth doing once, because a stick that wrote short produces a failure that looks
like a kernel bug. On Linux or macOS:

```bash
sudo dd if=/dev/sdX bs=512 count=$(( $(stat -c %s iso/boltusb.img) / 512 )) \
    2>/dev/null | cmp - iso/boltusb.img
```

Silence means the stick matches the image.

---

## Firmware setup

Get into firmware setup (usually <kbd>Del</kbd>, <kbd>F2</kbd>, <kbd>F10</kbd>
or <kbd>Esc</kbd> during POST) and set:

| Setting | Value | Why |
| --- | --- | --- |
| Secure Boot | **Disabled** | `BOOTX64.EFI` is not signed by anyone your firmware trusts. |
| Boot mode | **UEFI** preferred | Fewer moving parts than CSM. See below. |
| CSM / Legacy support | Enabled only if you need it | Only for the legacy path. |
| Fast Boot | Disabled | It often skips USB enumeration entirely. |

**Prefer UEFI over CSM if the legacy path is giving you trouble.** When the boot
menu lists your stick twice — `SanDisk Cruzer` and `UEFI: SanDisk Cruzer` — the
second one is the more *reliable* bet. In UEFI mode the firmware reads the kernel
through its own block and filesystem drivers, the same code it uses for every
other OS on the machine, so it is heavily exercised. The CSM path instead asks a
legacy BIOS emulation layer to pretend to be a 1985 disk controller while it
drives a modern USB stack underneath, and that emulation is where the
interesting bugs live.

The trade-off, stated plainly because it is large: **UEFI boot is currently much
slower to reach the desktop.** Measured under QEMU on 2026-09-14, same image,
same devices:

| Path | Time to the desktop |
| --- | --- |
| Legacy BIOS | 13 s |
| UEFI | 162 s |

Almost all of the difference is one phase — unpacking five binaries into the
in-memory filesystem, 8 s against 148 s. The boot is correct either way; it is
bulk memory work that is slow. Nothing in BoltOS programs MTRRs or PAT yet, so
the kernel runs on whatever memory type the firmware left behind, and OVMF does
not leave it write-back the way SeaBIOS does. That is a bug in BoltOS, not in
your machine, and it is worth knowing before you conclude a UEFI boot has hung:
give it three minutes before you decide.

So: try legacy first if your board still has CSM, and move to UEFI if the legacy
path fails or your board has no CSM at all.

Secure Boot has to be off either way. There is no signed shim for BoltOS and no
plan for one.

---

## What a good boot looks like

Within a second or two of picking the stick:

```
BoltOS S2: stage 2 loaded.
  boot header found

  ==== BoltOS boot menu ====
   1) BoltOS            (normal)
   2) Safe mode         (no desktop, no network)
   3) Recovery shell    (safe + read-only filesystem)
   4) BoltOS + fsck     (check the filesystem first)
  starting entry 1 in 5s ...
```

Press <kbd>1</kbd>, or wait five seconds. Then:

```
  VBE: setting the mode now
BoltOS stage 2
..................
BoltOS S2: kernel at 1MiB, going long mode.
  kernel verified: checksum matches
BoltOS S2: page tables built.
```

The dots are the kernel arriving from the stick, one per half-megabyte. On a
slow USB 1.1 port this stretches to twenty or thirty seconds, which is normal
and is exactly why the dots are there.

`kernel verified: checksum matches` is the line to look for. It means every
byte of the kernel made it from the stick into memory intact. The screen then
goes to the BoltOS desktop, or to a text shell in safe mode.

Note that the console changes hands partway through: the lines before
`BoltOS stage 2` are drawn by the firmware's text console, and everything after
it is drawn by BoltOS directly into the framebuffer, in the firmware's own font.
A brief flicker and a change of typeface there is the video mode being set, not
a fault.

---

## When it does not boot

### The machine never offers the stick

The stick is not being enumerated at all. In order:

- Use a **rear** USB port, directly on the motherboard. Front-panel headers and
  hubs are the single most common cause.
- Try a **USB 2.0** port if the board has one. Some firmware enumerates xHCI
  late or not at all before handing over.
- Turn **Fast Boot** off.
- Re-seat and re-write the stick; a short write leaves no valid boot sector.

### "No bootable device" / the machine skips straight past it

The firmware found the stick but not a boot sector it liked.

- Make sure you wrote **`boltusb.img`**, not `os.img`. This is the specific
  symptom of writing `os.img` to a physical stick.
- Make sure Rufus was in **DD mode** and did not write its own bootloader.
- In UEFI mode, confirm **Secure Boot is off**.

### It stops with a coloured bar and no text

A solid bar in the top-left corner and nothing else means the boot got as far as
long mode and then jumped into a kernel that was not there. Those bars are
breadcrumbs, painted directly into the framebuffer at each handoff:

| Bar | Painted by | Means |
| --- | --- | --- |
| red | stage2 | 64-bit mode reached, about to jump to the kernel |
| green | `kernel/boot.asm` | the kernel's entry stub is running |
| blue | `kernel/boot.asm` | `.bss` cleared, about to call C |
| yellow | `kmain` | C code is running |

They appear left to right, and the last one you see is the last stage that ran.
**Red and nothing else** is the case this guide was written for: stage2 finished,
and what it copied to 1 MiB was not a kernel.

If you see that on a current build, the checksum test above should have caught it
first and printed a message instead — so a bare red bar now means you are running
an image built before that check existed. Rebuild and rewrite the stick.

### "the kernel did not survive the trip from disk"

```
  checksum mismatch -- reloading one sector at a time
..................
BoltOS S2: the kernel did not survive the trip from disk.
  loaded 0x1A2B3C4D  expected 0x5E6F7A8B
```

stage2 read the whole kernel, INT 13h reported success for every one of those
reads, and what arrived does not match what was written. The image is fine; the
firmware mis-served the device. This is a real failure mode on USB-HDD emulation
and it is the reason the checksum exists.

Note the middle line: a mismatch is not fatal on the first try. stage2 reloads
the whole kernel a second time at one sector per read, because a BIOS that
mis-serves a 64-sector extended read while handling a single-sector one
correctly is the common shape of this bug. If you see the dots run a second time
and then the boot continue, that is what happened — the machine is fine, it is
just slow to start.

If it fails *both* times, in order of what usually works:

1. **Boot the stick in UEFI mode instead of CSM.** This skips INT 13h entirely.
2. A different USB port — rear, and USB 2.0 rather than 3.x.
3. A different stick. Some controllers are worse than others under CSM.

### "INT 13h will not read LBA 0x…"

The firmware refused a read outright, and kept refusing after a controller reset
and after backing the request down to a single sector. Either the stick has a
bad area, or the image on it is shorter than the boot header says it should be.
Re-write the stick and verify it with the `cmp` above.

### "A20 went back off and will not re-enable"

Rare, and firmware-specific: something in the BIOS turned the A20 line back off
behind our backs, which silently aliases everything above 1 MiB. Nothing to do
from here except boot in UEFI mode instead.

### Getting the boot log off the machine

stage2 writes everything it prints to **COM1 as well as the screen**, at
115200 8N1, so a machine that dies before the kernel starts can still be
questioned. If the box has a serial header — or you have a USB-TTL adapter and
a board with a pin header — connect it and capture with `screen /dev/ttyUSB0
115200` or PuTTY. You get the whole boot including the parts that scroll away.

If there is no UART, stage2 detects that (it probes the scratch register) and
skips serial entirely rather than stalling on every character.

### It boots but the screen stays black, or shows only text

Look for a line starting with `VBE:`:

```
  VBE: no 32bpp LFB mode offered -- kernel will be serial-only
```

The GPU's legacy VESA support has no 32-bpp linear-framebuffer mode, which is
common on newer cards in CSM mode. The kernel is running fine and talking to
COM1, there is just nothing to draw on. **Boot in UEFI mode** — the UEFI path
gets its framebuffer from GOP and does not depend on legacy VESA at all.

### The desktop appears and then the machine is sluggish or falls over

Check the memory line on the serial console or in `free`:

```
[pmm] 1048576 frames managed (4096 MiB), 783031 free (3058 MiB)
```

If "free" is near zero on a UEFI boot, you are running a build from before the
E820 stride fix. Rebuild.

---

## Installing to the internal disk

Once BoltOS is up off the stick, it can write itself to a real disk. Open a
terminal and run `install` with no arguments — it lists the disks it is willing
to write to, and what it would do to each:

```
BoltOS installer

  kernel image: 9.1M (read back from the boot medium)

  Installable disks:
    sda      ssd   238G     Samsung SSD 860 EVO
             -> boot chain + kernel 10 MiB, EFI partition 64 MiB, BoltFS 237 GiB

  install DISK                       (asks for an account)
  install DISK USER PASS [ESP_MB]    (unattended)
```

Then:

```
install sda
```

This **erases the whole target disk** — no dual boot, no resizing. It writes the
same layout the stick has: boot chain in the MBR gap, an ESP with
`BOOTX64.EFI`, and a BoltFS partition with your account on it. The disk you
booted from and the one holding the running filesystem are both excluded from
the list, so you cannot install over the stick you are running from.

`diskinfo` shows every disk the machine found, installable or not, which is the
command to reach for when the disk you expected is not on that list.

---

## How the boot chain actually works

Useful when something in the middle breaks.

**Legacy / CSM path.** Firmware reads LBA 0 into `0x7C00` and runs it.
`boot/stage1.asm` is 446 bytes of code sharing that sector with the partition
table; it uses INT 13h extended reads to pull LBA 1..32 into `0x8000` and jumps
there. `boot/stage2.asm` then enables A20, shows the boot menu, reads the boot
header at LBA 33 to learn where the kernel is and what it should check-sum to,
asks VESA for a 32-bpp linear framebuffer, collects the E820 memory map, enters
unreal mode, and copies the kernel from the stick to physical 1 MiB in 32 KiB
chunks through a bounce buffer at `0x70000`. Then it re-tests A20, verifies the
checksum, builds page tables — identity 0-4 GiB, a direct physical map, and the
higher-half kernel mapping — enters long mode and jumps to `0x100000`.

Three pieces of that are there specifically because real USB firmware is not
QEMU, and each one guards a failure that produces a hang rather than an error:

- **Unreal mode is re-established before every copy.** A CSM's INT 13h goes in
  and out of protected mode with our segment registers in its hands, and a BIOS
  that saves and restores `FS` with `push`/`pop` reloads the descriptor cache
  from a real-mode selector on the way back — silently undoing the flat 4 GiB
  limit that the copy to 1 MiB depends on.
- **Both ends of the bounce buffer are poisoned before each read.** A BIOS that
  returns success without transferring anything is a real thing on USB-HDD
  emulation, and without the sentinels it is indistinguishable from a good read.
- **The request shrinks on failure**, 64 sectors down through 8 to 1, with a
  controller reset between tries.

**UEFI path.** Firmware mounts the ESP, runs `/EFI/BOOT/BOOTX64.EFI`
(`boot/uefi_boot.c`), which opens `kernel.bin` on the same volume, asks GOP for
a framebuffer, converts the UEFI memory map into E820 entries, calls
`ExitBootServices`, installs the same page tables, copies the kernel to 1 MiB
and jumps to the same place. No INT 13h, no VESA, no unreal mode.

Both paths arrive at `kernel/boot.asm:_start` in long mode with `RDI` pointing
at a `struct bootinfo` (`include/boot.h`), which is the only thing the kernel
knows about how it was booted.
