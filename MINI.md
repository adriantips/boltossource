# BoltOS Mini

BoltOS Mini is BoltOS for small ARM boards. The first one is the **Orange Pi
Zero3** (Allwinner H618, four Cortex-A53 cores, 1 GB). Like the rest of
BoltOS it's written from scratch, here as an AArch64 kernel of about 600 KB.
It's up about a quarter of a second after U-Boot hands over (about five
seconds from plugging in), and it runs five apps:

- **Terminal**: a small shell (`help` lists the commands).
- **Monitor**: processor load, memory, the link to your Mac, USB devices.
- **Notes**: a page to type on.
- **Clock**: the time, large, and a stopwatch.
- **Settings**: wallpaper and accent, screen-sharing password, date and
  time, About, restart and power off.

The desktop edition's other 38 apps aren't here. There's no browser, no
compilers and no games, so nothing is competing with these five for a 1 GHz
core.

Until your LCD arrives, **your Mac is the Pi's screen**. The Pi's USB-C port
does double duty: it takes power from the Mac and shows up there as a
network adapter. BoltOS Mini runs a VNC server on that link, and macOS's
built-in Screen Sharing app shows the screen. The keyboard on the Pi's hub
works, and so do the Mac's keyboard and trackpad.

## What you need

- An Orange Pi Zero3 and a microSD card (any size; Mini uses the first
  130 MB of it).
- A **USB-A to USB-C cable that carries data**, not only power. Its USB-C
  end goes into the **Pi's USB-C socket**, and its USB-A end into a USB-A
  port on the Mac's side: on Apple's USB-C Digital AV Multiport Adapter,
  that's the rectangular USB-A port. The adapter's own USB-C socket only
  passes charging power through, so the Pi lights up there but the Mac never
  sees it. A USB-C to USB-C cable straight into a MacBook often doesn't
  power the Pi at all, because of how the Zero3 wires its USB-C socket.
- A USB keyboard (optional), on the **Pi's USB-A port**, through a hub if
  you like. That port makes the Pi the host. Don't use it for the cable to
  the Mac.
- A way to write the card once: a microSD reader on the Mac.

## Installing it

1. Build the card image, or use the one already in `build/mini/sdcard.img`:

   ```bash
   bash build-mini.sh
   ```

2. Put the card in the reader and find its name (`disk4` or similar):

   ```bash
   diskutil list external
   ```

3. Write it. The script only accepts external disks, shows the disk it's
   about to erase, and asks first. The write needs your Mac password,
   because only root may write a raw disk.

   ```bash
   bash mini/tools/flash-card.sh disk4
   ```

4. Put the card in the Pi and plug the Pi's USB-C socket into the Mac's
   USB-A port, as described above.

After this one time, the card stays in the Pi. New versions go onto it over
the same USB-C cable (see "Updating it" below).

### The first boot

- The **red** light is power.
- The **green** light comes on for two seconds while U-Boot (the Pi's
  firmware) waits to see whether the Mac wants to send it anything. It goes
  off while BoltOS starts, and stays on once BoltOS is running.
- macOS may ask **"Allow accessory to connect?"**. Click **Allow**. It may
  ask twice: once for U-Boot's firmware-update device, and once for
  "BoltOS Mini" itself.
- A few seconds later a new network adapter called **BoltOS Mini** appears in
  System Settings → Network. It has the address 10.77.0.2, and the Pi is
  10.77.0.1. Your Mac keeps using Wi-Fi for the internet; the Pi doesn't
  offer it a route.

## Seeing the Pi's screen on your Mac

There are three ways in. All of them show the same screen.

**From Finder.** Open Finder and choose **Go → Connect to Server…** (⌘K).
Type `vnc://10.77.0.1` and click Connect. When it asks for a password, type
`boltos`. The Pi also announces itself, so it can show up in the Finder
sidebar under Network as "BoltOS Mini", with a **Share Screen…** button.

**From the terminal, in one step.** This also sets the Pi's clock from the
Mac's, because the Zero3 has no battery-backed clock of its own:

```bash
python3 mini/tools/pi.py screen
```

**The web page.** Open <http://10.77.0.1> in Safari. It has an **Open the
screen** button, and **Set it from this Mac** for the clock.

Once the window is open:

- Type and click in the window to use BoltOS Mini with the Mac's keyboard
  and trackpad. The keyboard on the Pi works at the same time.
- **View → Scaling** in Screen Sharing makes the window larger or smaller.
  The screen is 1024 × 600, the size of a 7" LCD.
- Closing the window doesn't stop anything; reopen it whenever you like.
- The password can be changed in Settings → Screen Sharing, or with
  `passwd` in the Terminal. A changed password lasts until the next
  restart.

### Without the Pi at all

The same kernel runs in QEMU on the Mac. It opens in a window of its own:

```bash
bash build-mini.sh run          # emulates the A53, like the Pi
bash build-mini.sh run --hvf    # runs on the Mac's own cores: much faster
```

That copy also has VNC on `vnc://localhost:5901` (password `boltos`) and the
web page on <http://localhost:8080>. `--ohci` adds a USB hub with a
keyboard and mouse behind it, which is the setup your mini keyboard uses.

## Using it

| Key | Does |
| --- | --- |
| ← → and Return | choose an app on Home, and open it |
| 1–5, or F1–F5 | open an app directly |
| Esc, or the Windows/⌘ key | back to Home |
| ↑ ↓ ← → in Settings | move through the list, change a value |
| Space, R in Clock | start/stop and reset the stopwatch |

Some Terminal commands: `about`, `net`, `usb`, `mem`, `cpu`, `log 40`,
`led green off`, `time 18:30`, `theme dawn`, `reboot`, `poweroff`.

## Updating it

Every time the Pi starts, U-Boot spends two seconds as a firmware-update
(DFU) device on the USB-C port. `pi.py` uses that window. If BoltOS is
running, it asks it to restart first, so nobody has to touch the Pi.

```bash
bash build-mini.sh                                     # build a new kernel
python3 mini/tools/pi.py install build/mini/boltos.bin # put it on the card
```

For trying a kernel without keeping it, use `pi.py boot`. It runs the
kernel from RAM, and the card's copy comes back on the next restart.
`bash build-mini.sh pi` is the build plus `pi.py boot` in one step.

The DFU commands need pyusb and libusb. `build/.tools/env` has both; use
`build/.tools/env/bin/python3` to run `pi.py`.

Other `pi.py` commands: `log` (BoltOS's log; if BoltOS isn't running, the
log the last run left in RAM, fetched through U-Boot), `shot` (a
screenshot), `time`, `reboot`.

## When something is wrong

- **Nothing appears on the Mac.** Check that the cable carries data: a
  phone-charging cable often doesn't. Check for an "Allow accessory" prompt
  that hasn't been answered. BoltOS no longer restarts when it has no Mac, so
  it also runs from a battery or a power bank. To send it a new kernel, plug
  the Mac in and use `pi.py boot` (it reboots the Pi into U-Boot's window).
- **The green light blinks fast.** BoltOS stopped on an error. The watchdog
  restarts the Pi within 16 seconds. `pi.py log` then shows what happened.
- **Screen Sharing says the password is wrong.** It's `boltos` unless it was
  changed since the Pi last started.
- **The clock says "not set".** Run `pi.py screen` or `pi.py time`, open
  <http://10.77.0.1>, or set it in Settings → Date & Time.

## When the LCD arrives

The screen is a buffer in RAM, and anything can show it. Today that's VNC
and QEMU's window. An LCD needs one more driver behind `screen_present()`
(`mini/screen.c`). Which driver depends on the panel: a small SPI panel on
the pin header (ILI9341, ST7789, ILI9486) is a short driver, and the H618's
HDMI output is a much larger one. The layout scales with screen height
(`S()` in `mini/ui.h`), so 480 × 320 and 800 × 480 work without changes.

## How it works

```
 boot ROM ─▶ U-Boot SPL (DRAM) ─▶ TF-A ─▶ U-Boot ─▶ boltos.bin (arm64 Image)
                                            │ 2 s DFU window on USB-C
                                            └ loads BOLTOS.BIN off the card
```

- **Firmware**: `mini/firmware/`. U-Boot 2026.07 with TF-A 2.15, built by
  `build-uboot.sh`. The changes to the stock Orange Pi Zero3 configuration
  are in `uboot-mini.config`: the DFU window, the watchdog, and the boot
  command.
- **The machine**: `mini/arch/` holds the entry (EL2 → EL1), the identity
  map with caches on, exceptions, GICv2/v3 and the 1 kHz timer.
  `mini/board.c` tells the Pi and QEMU apart from the device tree and
  covers the UART, LEDs, watchdog and PSCI.
- **USB-C, as a device**: `mini/musb.c` drives the H618's MUSB controller
  as a CDC-ECM network adapter, which macOS supports with no drivers.
- **USB-A and the header ports, as a host**: `mini/usbhost.c` drives OHCI,
  with hubs and boot-protocol keyboards and mice. EHCI is left off, so a
  USB 2.0 hub runs at full speed and passes keyboards through.
- **Network**: `mini/net.c` is a small IPv4 stack whose TCP keeps a window
  of segments in flight. The desktop's TCP sends one segment at a time,
  which is too slow to stream a screen. `mini/netsvc.c` adds DHCP, mDNS, the
  web page and pi.py's endpoints. `mini/vnc.c` is RFB 3.8 with Hextile.
- **Screen and shell**: `mini/screen.c`, `mini/shell.c` and the `app_*.c`
  files, drawn with the same glass layer and fonts as the desktop
  (`kernel/gfx.c`, `kernel/font.c`).
- **QEMU**: `mini/qemu.c` (ramfb, PCI for the OHCI test rig) and
  `mini/virtio.c` (network, keyboard, tablet).

Everything runs in one loop on one core: poll each device, run the network,
draw a frame if something changed, feed the watchdog, sleep until the next
tick. Nothing waits on anything else.

## Building it

```bash
bash build-mini.sh
```

This uses the same toolchain as the TV edition: clang and ld.lld from
`build/.tools/env` (see TV.md, "Building it"), because clang targets AArch64
without anything extra. The output is `build/mini/boltos.bin` (the kernel)
and `build/mini/sdcard.img` (a whole card). U-Boot comes prebuilt in
`mini/firmware/`. `mini/firmware/build-uboot.sh` rebuilds it, and needs
Arm's aarch64 GCC, which the script downloads.
