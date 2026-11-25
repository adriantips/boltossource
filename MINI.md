# BoltOS Mini

BoltOS Mini is BoltOS for small ARM boards. The first one is the **Orange Pi
Zero3** (Allwinner H618, four Cortex-A53 cores, 1 GB). Like the rest of
BoltOS it's written from scratch, here as an AArch64 kernel of about 2.7 MB
(most of it fonts and the wireless chip's firmware). It's up about a quarter
of a second after U-Boot hands over (about five seconds from plugging in),
and it has eight apps built in:

- **Browser**: web pages, DuckDuckGo search with All, Images and Videos
  tabs, and YouTube, decoded on the Pi's second core (no sound yet).
- **Code**: an editor that runs Python, C and C++ on the Pi (see below).
- **Terminal**: a small shell, with the SD card's files (`help` lists the
  commands).
- **Monitor**: processor load, memory, the link to your Mac, USB devices.
- **Notes**: a page to type on, kept on the SD card.
- **Clock**: the time, large, and a stopwatch.
- **Settings**: Network (Ethernet), Bluetooth, theme and accent, screen-sharing
  password, date and time, About, restart and power off.
- **Packages**: apps from boltos.dev that aren't in the kernel, installed
  over the network onto the SD card. The first is **BoltOS Local AI**, a
  chat with a language model that runs on the Pi itself (see below).

Files are kept on the card's FAT32 partition, the one BOLTOS.BIN is on, so
a Mac with the card in its reader sees the same folders.

The Pi gets online by Ethernet only (its Wi-Fi is off: see below), and Bluetooth
keyboards, mice and game controllers work alongside USB ones.

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

A bar runs along the top: **BoltOS** (Home) and the apps by number on
the left, the network, processor load and the clock on the right. Clicking
an app there opens it.

| Key | Does |
| --- | --- |
| ← → and Return | choose an app on Home, and open it |
| 1–9, or F1–F9 | open an app directly (1 is the Browser, 2 Code, 7 Settings, 8 Packages) |
| Esc, or the Windows/⌘ key | back to Home |
| ↑ ↓ ← → in Settings | move through the list, change a value |
| Space, R in Clock | start/stop and reset the stopwatch |
| Space, ← → on a video | pause and play, ten seconds back and on |

Some Terminal commands: `about`, `net`, `usb`, `mem`, `cpu`, `log 40`,
`led green off`, `time 18:30`, `theme light` (or `dark`, `black`), `reboot`,
`poweroff`. For the SD card's files: `ls`, `cd`, `pwd`, `cat`, `mkdir`, `rm`
(a folder goes with what's in it), `mv`, `cp`, `touch` and `df`; Tab
completes a name. `edit file` opens a file in Code, and `run file.py` (or
`.c`, `.cpp`) runs it there.

### Saving a picture and showing it full screen

In the Browser (Mini's own pages, not Firefox, which has its own menu),
**right-click a picture**: a page's, a thumbnail on Images, or the one open
over the grid. Pick **Save image**, type a name (letters, numbers, `-` and
`_`) and press Return. Mini downloads the picture as it is (PNG, JPEG, GIF
or BMP) into `/Pictures` on the SD card. Then it tells you how to show it:

```
img --kitty
```

Type that in the Terminal and the picture fills the screen. **Ctrl+C closes
it** and takes you back to the Terminal. Esc works too, and so does a click on
the line at the bottom that says so. That line shows for a few seconds, and
any other key or click brings it back. `img` on its own lists the saved pictures.

While a picture is up, Mini does nothing else. It draws the picture once, stops
Firefox's machine, skips package and update checks, and keeps the screen
from going to sleep.

## Updating it

BoltOS Mini updates itself from boltos.dev. It asks once it's online and
the clock is set, and every six hours after. Settings → Updates says
which version is running and which is the newest, with what's in it.

- **Automatic updates on** (the default): a *quick* update found when the
  Pi starts downloads, goes onto the card and the Pi restarts into it,
  before you've opened anything. One found later waits for the next start.
- A *big* update, or any update with Automatic updates off, waits: the bar
  says `update 1.2.0` (click it), and Return on **Update to 1.2.0** in
  Settings → Updates installs it and restarts.

The download has to match its SHA-256. The new `BOLTOS.BIN` is written
beside the old one and read back before one directory sector points at it,
so a power cut leaves one kernel or the other. If the Pi restarts for an
update and comes back on the old version, it never tries that version by
itself again (Update still offers it).

The kernels boltos.dev hands out are built without the Wi-Fi chip's
firmware (it isn't ours to give away). A kernel that has it keeps a copy
in `System/Firmware` on the card at every start, and an updated kernel
loads it from there, so Wi-Fi and Bluetooth carry on. A card that never
ran a kernel with the firmware has no copy; copy `wcnmodem.bin`,
`wifi_ini.bin`, `bt_pskey.bin` and `bt_rf.bin` from `build/mini-fw/uwe5622/`
of a build that fetched them into `System/Firmware` to get them.

Publishing an update is `bash release.sh mini`: it raises `mini/VERSION`,
builds the kernel and puts it on boltos.dev (see `release.sh --help`;
`--big` marks a big one). The dev endpoint `/update` shows where updates
stand, and `/update?check`, `?install` and `?auto=0|1` drive them.

### From the Mac

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

### How fast it is

<http://10.77.0.1/perf> says, since the last time it was asked, how many
frames the shell drew and what they cost, how long the main loop's longest
turn was, and how many updates each screen viewer was sent. Two more
development pages help measure: `/bench?n=30` draws the current screen 30
times and reports the cost of one, and `/anim?mode=0&ms=5000` changes the
whole screen every frame for five seconds (`mode=1` only moves Home's
selection). `mini/tools/vncbench.py` is a viewer that logs in, keeps
asking for updates while `/anim` runs, prints what arrived, and with
`--verify` checks the picture pixel by pixel against `/screen.bmp`:

```bash
build/.tools/env/bin/python3 mini/tools/vncbench.py --enc zlib --depth32 --verify
```

On the Zero3 at 1008 MHz, Screen Sharing gets about 59 frames a second
when the whole screen changes every frame, 60 when part of it does, and
every frame of a 30 fps YouTube video.

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

## Wi-Fi and Bluetooth

The Zero3's wireless chip (a Unisoc UWE5622) needs its firmware loaded by
the Pi at every start. `build-mini.sh` downloads it, with its Wi-Fi and
Bluetooth settings, from Orange Pi's firmware repository at a fixed commit
and checks it against known hashes; the files aren't part of this
repository. Built offline, Mini works without Wi-Fi and Bluetooth.

**Wi-Fi is off.** The Zero3's Wi-Fi antenna snapped off, so Mini relies
on the Ethernet socket alone: Wi-Fi never starts by itself and Settings has
no Wi-Fi page; Settings → Network shows the cable's speed, the address the
router gave, the router and DNS. `mini/wifi.c` is still built (open and
WPA2 networks, the last one kept on the card) and the development endpoint
`/wifi?up` starts it by hand.

**Bluetooth**: switch it on in Settings → Bluetooth and put the device in
pairing mode; it shows under Other Devices. Return pairs it. A keyboard
shows a six-digit code: type it on the keyboard, then Enter. Mice and game
controllers just pair. Paired devices are under My Devices and reconnect
by themselves; Backspace twice forgets one. Classic and Bluetooth LE
keyboards, mice and controllers work; a controller's D-pad or left stick
is the arrow keys and its first four buttons are Return, Esc, Backspace
and Tab. Headphones and speakers can't be used yet, as Mini has no sound
output.

## Code: Python, C and C++

**Code** (2, or F2) is an editor for programs, and runs them on the Pi.
The first time it opens it makes a `Code` folder on the card with three
examples, `hello.py`, `hello.c` and `hello.cpp`; the files are listed on
the left, open ones are tabs along the top.

| Key | Does |
| --- | --- |
| F5, or Ctrl+R | save the file and run it (again to stop it, or Esc) |
| Ctrl+S, Ctrl+Shift+S | save, save as |
| Ctrl+N, Ctrl+Shift+N | a new file (`name.py`, `name.c`, `name.cpp`), a new folder |
| Ctrl+O, Ctrl+B | go to the file list, hide or show it |
| Ctrl+W, Ctrl+Tab | close the file, go to the next one |
| Ctrl+Z, Ctrl+Y | undo, redo |
| Ctrl+C, Ctrl+X, Ctrl+V | copy, cut and paste (a whole line when nothing is chosen) |
| Ctrl+F, Ctrl+H, F3 | find, replace (Ctrl+Return replaces them all), find again |
| Ctrl+G | go to a line |
| Ctrl+/ | comment the lines out, or back in |
| Ctrl+D, Ctrl+Shift+K, Alt+↑ ↓ | copy the line, remove it, move it |
| Tab, Shift+Tab | indent, take the indent off (of every chosen line) |
| Ctrl+J, Ctrl+L | hide or show the output, clear it |

The Mac's ⌘ works wherever Ctrl does, and the clipboard is shared with the
Mac through Screen Sharing (Edit → Use Shared Clipboard), so code copied on
the Mac pastes into the Pi and the other way round. Shift with the arrows,
or the mouse, chooses text; a double click chooses a word. Enter keeps the
indentation, and adds a level after a `:` in Python or a `{` in C. In the
file list, `n` makes a file, `f` a folder, `r` renames and Delete removes.

A program runs on one of the Pi's spare cores, so the screen, the network
and Stop carry on whatever it does. What it prints shows under the editor
as it comes; when it asks for input (`input()`, `scanf`, `cin`), type in
the output and press Return. An error is shown there, the line it happened
on is marked in the text, and a Python error has its traceback. A program
gets up to about 360 MB, with a garbage collector for Python.

- **Python** is Python 3: integers of any size, floats, strings, lists,
  tuples, dicts and sets; functions with defaults, `*args` and `**kwargs`;
  closures, lambdas and decorators; classes with inheritance, `super()`,
  properties and the special methods; `try`/`except`/`finally`, `with`;
  comprehensions, f-strings, `format()` and `%`; slicing, unpacking and
  `:=`. Modules: `math`, `random`, `time`, `sys`, `os` (`os.path`),
  `string`, `json`, `collections` (`Counter`, `defaultdict`, `deque`,
  `namedtuple`), `itertools`, `functools`, `dataclasses` and `typing`, and
  the program's own `.py` files next to it. `open()` reads and writes files
  in its folder on the card. Not there yet: generators (`yield`), `async`,
  and `match`.
- **C** is C as a compiler sees it: types, pointers and pointer arithmetic,
  arrays, structs, unions, enums, typedefs, function pointers, the
  preprocessor (`#define`, `#if`, and `#include "x.h"` for your own
  headers), `printf`/`scanf` and the rest of `stdio.h`, `string.h`,
  `stdlib.h` (`malloc`, `qsort`, `rand`...), `math.h`, `ctype.h`. Memory is
  laid out as a real program's, so a null or wild pointer is a
  "Segmentation fault" with its line, and so is running out of stack. No
  `goto`.
- **C++** adds classes with constructors, destructors, inheritance and
  virtual methods; references; `new` and `delete`; operator and function
  overloading, default arguments; templates; lambdas; `auto` and range-for;
  `try`/`catch`/`throw`; and from the library `string`, `vector`, `map`,
  `set`, `pair`, `stack`, `queue`, `priority_queue`, `iostream` with
  `iomanip`, and the common `<algorithm>`s (`sort`, `find`, `count`,
  `reverse`, `accumulate`, `min`/`max`, `lower_bound`...). Not there:
  `stringstream`, reverse iterators, and multiple inheritance.

These are interpreters written for Mini, not CPython and not a compiler.
On the Zero3, `fib(25)` (a quarter of a million calls) takes 0.31 s in
Python and 0.25 s in C. Numbers print exactly as CPython and a C library
do (`0.1 + 0.2` is `0.30000000000000004`), and `math` is correct to the
last digit or so.

## Files on the SD card

Mini keeps files on the card's FAT32 partition, the one U-Boot loads
BOLTOS.BIN from:

- `Code/`: Code's programs.
- `Notes/notes.txt`: Notes' page, saved a moment after you stop typing.
- `System/keep.bin`: what has to outlive a power cut: the Wi-Fi network,
  Bluetooth pairings, the theme and accent, the time zone.

Names can be long and mixed-case, and folders nest. A file is written into
free space first and only then put in place, so a power cut while saving
leaves the old file whole. Take the card out and a Mac shows the same
folders (it may add `._` files, which Mini doesn't show). BOLTOS.BIN itself
can't be removed or replaced from the Pi; `pi.py install` does that.

## Packages and BoltOS Local AI

**Packages** (7, or F7) lists the apps boltos.dev offers
(<https://boltos.dev/pkg/index.json>). Return installs the chosen one: it
downloads over Ethernet, its SHA-256 is checked against the list,
and it's kept on the SD card. It then shows up in the bar after the
built-in apps, and is loaded again at every start. Return on an installed
one opens it; Backspace twice removes it, with everything it downloaded.

Nothing on the card's partitions is touched: installed things live in a
*store* that starts at 256 MiB into the card (or past the last partition,
if that's further), in space a card image never uses. A card smaller than
about 400 MB has no room for one.

**BoltOS Local AI** is a chat with a language model, run on the Pi's own
cores with no cloud and no account. The first time it opens, it shows its
models; pick one and Return downloads it from Hugging Face, straight onto
the card:

| Model | From | Download | Answers at | Loads in |
| --- | --- | --- | --- | --- |
| Qwen3 0.6B (Q8_0) | `Qwen/Qwen3-0.6B-GGUF` | 639 MB | 4.2 tokens/s | 32 s |
| SmolLM2 360M Instruct (Q8_0) | `HuggingFaceTB/SmolLM2-360M-Instruct-GGUF` | 386 MB | 7.5 tokens/s | 23 s |
| Qwen2.5 0.5B Instruct (Q8_0) | `Qwen/Qwen2.5-0.5B-Instruct-GGUF` | 676 MB | 5.6 tokens/s | 34 s |

Measured on the Zero3 at 1008 MHz. Those are the models that fit the Pi's
1 GB and answer at three tokens a second or more; bigger ones don't. They
are the 8-bit (Q8_0) files from each maker's own repository: the 4-bit
ones are smaller and only a little faster here (the A53 is short of
arithmetic, not of memory), and they answer noticeably worse. A
question goes in at 10-15 tokens a second, so one with web search results
in front of it takes half a minute or so before the answer starts.

Downloads come over Ethernet at about 1 MB/s. A download that
stops carries on where it got to, and once it's whole it is read back and
checked against Hugging Face's SHA-256. Return on a downloaded model loads
it, and it's loaded again the next time Local AI opens.

In the chat, type and press Return. Esc stops an answer; Ctrl+L starts a
new chat; PgUp and PgDn scroll. Tab goes to the models and settings:

- **Web search** is *Auto*, *Always* or *Off*. Auto searches when a
  question is about today, the news, the weather, prices, scores or "the
  latest". The search goes to Exa's hosted MCP server
  (`https://mcp.exa.ai/mcp`, its `web_search_exa` tool, no key needed); the
  top three results go in front of the question, and are listed above the
  answer. Exa sees the questions that are searched.
- **New chat** forgets the conversation. When the model's memory (2048
  tokens, 1536 for Qwen3) fills up, it starts over by itself.

Models this small answer everyday questions well and get facts wrong now
and then. Web search is what helps most with anything recent.

While a model is loaded it holds both of the big windows of RAM. Playing a
video needs one of them, so starting a video puts the model away, and it
loads again when you come back to Local AI. The other way round, loading a
model stops a video whose page isn't on screen; Space on that page plays it
again.

## Firefox

The **Browser** (1) is real Firefox, running on the Pi: pages look exactly as
Firefox on Windows draws them -- React apps, web fonts, video and all --
because that is what draws them. Its window fills the space under the bar:
its own tabs, address bar, bookmarks, history, downloads, extensions.

Firefox needs Linux under it, so BoltOS runs Linux as a guest, on its own
hypervisor: U-Boot starts Mini at EL2, the A53's hypervisor level, and Mini
keeps that level for itself. The guest -- *the Firefox machine* -- gets cores
1-3 and the two big windows of RAM (894 MB); BoltOS keeps core 0 and makes
its devices: a disk (Firefox and Debian, read-only), a second disk for the
profile, a screen, a keyboard, a tablet, and a network card it routes through
Ethernet. Nothing else runs on the Pi; nothing needs a Mac.

The first time the Browser opens it offers to download Firefox (about 310 MB,
from boltos.dev, onto the card's store, checked against its SHA-256). It keeps
downloading while other apps are open. Then the first start reads Firefox off
the card; after that the machine stays up, and coming back to the Browser is
instant.

| Key | Does |
| --- | --- |
| everything | goes to Firefox: Ctrl+L, Ctrl+T, Ctrl+W, F5, Ctrl+F, ... |
| Esc | goes to the page (a menu, full-screen video) |
| Esc twice | back to Home |
| Q, while it starts | the quick browser instead (BoltOS's own engine) |

Firefox is set up as Firefox on Windows 11 (`mini/firefox/user.js`): a page
sees Windows' user agent and platform, gets Windows' default fonts --
Microsoft's core web fonts (Arial, Times New Roman, Verdana, Georgia, Courier
New...), which the machine downloads from Microsoft on its first start, and
Selawik and Cascadia Mono standing in for Segoe UI and Consolas -- Windows
11's scrollbars, and Windows' light theme. Video comes as H.264 where a site
offers it (YouTube does), which is what an A53 decodes fastest. There is no
sound: Mini has no audio output yet.

Settings for it don't exist yet; the dev page has `/vm` (state, `?start`,
`?stop`, `?con` for the guest's console).

**Firefox on another computer.** If a Mac or PC runs the BoltOS Web Engine
(`webengine/`: Firefox in a small Linux VM, streamed as H.264), the Browser
can show that one instead: `bash webengine/vm.sh pair` points a Mini at it
(`/engine?set=HOST:7310&key=K`, `/engine?off` to stop). It's off unless set.

**Building the Firefox machine.** `bash mini/firefox/make-image.sh` builds the
guest kernel (Linux 6.18 LTS) and root file system in an arm64 Debian VM on the
Mac (`webengine/vm.sh setup` makes the VM), into `build/mini/firefox/`;
`bash mini/firefox/publish.sh` puts them on boltos.dev. For a dev Pi,
`curl --data-binary @build/mini/firefox/root.sqfs "http://10.77.0.1/store?name=fx/root.sqfs"`
(and `fx/Image`) copies them straight onto its card over USB-C (about 4 MB/s,
70 s for the root). `/vm?debug` starts the next machine with a root shell
on its console: `/vm?sh=free+-m` types a line into it, `/vm?con` shows what
came back.

## A screen on the HDMI port

Plug a monitor into the Pi's micro-HDMI port (a micro-HDMI to HDMI cable or
adapter) and the screen shows up on it. It's the same screen the Mac sees,
so Screen Sharing keeps working alongside. Mini reads the monitor's EDID and
uses the size the monitor asks for, so a 1080p monitor gets 1920 × 1080 at
60 Hz. A monitor plugged in after Mini has started gets picked up within a
second. Unplugging it leaves the screen at the size it was.

Settings → Display picks the resolution. Left and right move through the
sizes the monitor supports, and Return switches. **Automatic** follows the
monitor. With no monitor attached, the list has sizes for Screen Sharing
instead (1024 × 600 up to 1920 × 1080). The choice is kept across restarts.
The layout scales with screen height (`S()` in `mini/ui.h`), so every size
looks the same, only bigger.

Sizes go up to 1920 × 1200. Two screen buffers have to fit in the kernel's
96 MiB heap, so 4K monitors get their largest mode at or under that size.
The signal is DVI-style RGB, so no audio is carried over HDMI.

The driver (`mini/hdmi.c`) sets up the H618's display engine (DE 3.3), TCON
TOP, TCON TV0 and the DesignWare HDMI controller and PHY itself, because our
U-Boot has no video. A small SPI screen on the 26-pin header has its own
driver (`mini/lcd.c`, below).

### An HDMI LCD

The 5" and 7" touchscreens sold for the Raspberry Pi (Waveshare, Elecrow,
SunFounder, Hosyond...) are HDMI monitors with touch on a separate USB cable,
and Mini drives both. The Pi's port is **micro-HDMI**, so the screen needs a
micro-HDMI to HDMI cable or adapter. Many come with one for the Pi 4 and 5.

- **Picture**: the panel's EDID gives its own timing (a 7" panel is 1024 × 600,
  Mini's design size; a 5" one is 800 × 480), and Automatic uses it. A board
  that never raises hot-plug detect is found by its EDID answering on the DDC
  wires. A board with no EDID gets 1280 × 720 (its scaler takes it), with
  1024 × 600 and 800 × 480 in Settings → Display at the CVT timings the makers
  give for the Pi.
- **Touch**: plug the screen's touch USB cable into the Pi's USB-A port (with a
  hub if the keyboard is there too). Any driver-free HID touchscreen works,
  the Windows multi-touch kind (`mini/usbhost.c` reads its report descriptor).
  The first finger is the pointer: **tap** to click, **slide** to scroll (the
  page follows the finger), **hold, then move** to drag (select text, move a
  slider). Settings → Display shows it under Touch; `/debug` dumps its report
  descriptor and its last reports.
- **Power**: a 7" panel draws about half an amp. If the Pi restarts when the
  screen lights up, power the screen from its own USB charger and keep only
  the touch cable on the Pi.

### A 3.5" SPI screen

The 3.5" 480 × 320 SPI modules with an **ST7796S** controller (sold for
Arduino and STM32 boards, with or without touch) go on the Zero3's 26-pin
header. Wire it with the Pi off:

| Module | Zero3 pin | | Module | Zero3 pin |
|---|---|---|---|---|
| 3V3 | 17 (3.3 V) | | GND | 20 |
| SCK | 23 (PH6) | | MOSI | 19 (PH7) |
| MISO | 21 (PH8) | | CS | 24 (PH9) |
| RS (DC) | 22 (PC7) | | RST | 18 (PC14) |
| BL | 16 (PC15) | | T_CS | 26 (PC10) |

The two 4-pin cables that come with some modules carry most of these; RST,
BL and T_CS are on the long header. RST is optional (Mini resets the panel
with a command too). Leaving BL unconnected keeps the backlight on all the time, so
screen sleep can't turn it off. T_CS is only for the touch version. IRQ
isn't needed, because the touch is polled. Power the module from 3.3 V, not 5 V.

At start, Mini resets the panel and asks it whether it's asleep. A panel that
answers gets the screen. **Automatic** in Settings → Display is then
480 × 320, and the HDMI port shows the same. With a monitor plugged in too,
the monitor sets the size and the small screen shows it scaled to fit. Only
the 16 × 16 tiles that changed are sent, as RGB565, by the H618's DMA
controller through SPI1 at 30 MHz. A full screen takes about 80 ms, and a
typical change a few. Screen sleep turns the backlight off.

Settings → Display has three rows for it:

- **SPI screen**: Automatic (use it when it answers), On (a module wired
  without MISO, which can't answer), or Off (Mini leaves the header's pins
  alone).
- **Upside down**: turns the picture round.
- **Touch** (when an XPT2046 answers on T_CS): Return shows three crosses on
  the small screen. Tap each one to calibrate. After that, tap, slide and hold work as
  they do on an HDMI LCD's touch.

`/lcd` (development) shows what the driver sees: the probe's answers, the
transfers, and the touch's raw readings. It also sets what a particular module may need,
kept on the card:
`?inv=0` if the picture looks like a negative (inversion is on by default,
which the TK035F5501 panel on these modules needs), `?rgb=1` if red and blue
are swapped, `?madctl=HEX` if the picture is mirrored, `?mhz=20` if long
wires give a noisy picture (`?mhz=50` for more speed on short ones),
`?fill=RRGGBB` for a test colour, `?mode=1` to force it on.

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
- **The hypervisor**: `mini/arch/hyp.S` is all that runs at EL2 -- a world
  switch, as KVM does on a CPU without VHE: BoltOS stays at EL1 and asks it
  to run a guest CPU with `hvc`. `mini/vm.c` is the guest's machine: stage-2
  translation over the two RAM windows, a vCPU per core on cores 1-3, PSCI,
  an emulated GIC distributor with the GIC-400's virtual CPU interface and
  list registers (the guest's timer goes in linked to the physical
  interrupt), a PL011 console into the log and a PL031 clock, and its
  device tree. `mini/vdev.c` makes its virtio-mmio devices: two disks on the
  store, virtio-gpu (2D), keyboard and tablet, an RNG, and a network card
  NAT'd through `mini/net.c`. `mini/fxget.c` downloads the machine's files.
  `mini/board.c` tells the Pi and QEMU apart from the device tree and
  covers the UART, LEDs, watchdog and PSCI.
- **USB-C, as a device**: `mini/musb.c` drives the H618's MUSB controller
  as a CDC-ECM network adapter, which macOS supports with no drivers.
- **USB-A and the header ports, as a host**: `mini/usbhost.c` drives OHCI,
  with hubs, keyboards and mice, and HID multi-touch touchscreens (an HDMI
  LCD's touch; `input_touch()` in `mini/input.c` turns a finger into taps,
  scrolls and drags). EHCI is left off, so a USB 2.0 hub runs at full speed
  and passes keyboards through.
- **The SPI screen**: `mini/lcd.c` drives SPI1 and the DMA controller for
  an ST7796S on the 26-pin header. It sends what changed in the screen by
  the tiles' generations, as the VNC server does, and polls an XPT2046 touch
  on the same bus.
- **Wi-Fi and Bluetooth**: `mini/sdio.c` drives the H618's MMC1 as an
  SDIO host for the UWE5622; `mini/wcn.c` loads its firmware and carries
  its packets. `mini/wifi.c` is the host side of its Wi-Fi firmware (scans,
  joining, WPA2's handshake with the desktop's `net/wpa.c`); `mini/bt.c` is
  a Bluetooth host (HCI, pairing, L2CAP, SDP, SMP, GATT) and `mini/hid.c`
  reads HID devices' reports. The build turns the vendor's `.ini` files into
  what the firmware takes (`mini/tools/mkwifiini.py`, `mkbtcfg.py`).
- **YouTube**: `mini/player.c` streams a video's 360p MP4 and `mini/h264*.c`
  decode it on the second core.
- **Network**: `mini/net.c` is a small IPv4 stack whose TCP keeps a window
  of segments in flight. The desktop's TCP sends one segment at a time,
  which is too slow to stream a screen. `mini/netsvc.c` adds DHCP, mDNS, the
  web page and pi.py's endpoints. `mini/vnc.c` is RFB 3.8. It sends each
  viewer the 16x16 tiles that changed since its last update (the screen
  stamps them as it presents), in the first encoding the viewer lists of
  ZRLE, zlib, Hextile and Raw. Screen Sharing lists zlib first; its
  deflate is a small one made for screens (runs and the row above, fixed
  Huffman codes), and photos go as stored blocks.
- **Files**: `mini/fs.c` is a FAT32 driver for the card's partition: long
  names (with 8.3 aliases and checksums, as Windows makes them), both FATs
  and FSInfo kept up, a directory changed in memory and written back by
  the sector.
- **Code**: `mini/app_code.c` is the editor. `mini/rt.c` runs a program on
  a spare core, on a stack of its own in the low big window of RAM, with
  `mini/gc.c`'s heap (a "big bag of pages", collected conservatively: any
  word that points into an object keeps it). Output goes back through a
  ring the boot core drains; input and file reads and writes are requests
  the boot core answers. `mini/py_*.c` is Python (tokens, a parser that
  works out where every name lives, a tree-walking evaluator, the
  library). `mini/cc.c` is C and C++'s front end (preprocessor, a parser
  that types every expression, templates by reading their tokens again),
  `mini/cc_eval.c` runs the typed tree over the program's own memory, and
  `mini/cc_lib.c` is its library. `mini/numfmt.c` prints and reads floating
  point exactly (Steele & White's digits, on big integers), and
  `mini/mathx.c` has `exp`, `log`, `pow` and the rest to within an ulp.
  `mini/tools/pyhost.c` builds the same interpreters on a Mac for testing.
- **Screen and shell**: `mini/screen.c`, `mini/shell.c` and the `app_*.c`
  files, drawn with the desktop's drawing layer and fonts (`kernel/gfx.c`,
  `kernel/font.c`) in a flat style: solid colours, hairlines, no blur or
  shadows, which also keeps a frame to 1-3 ms. `screen_present` copies
  only the 16-pixel pieces of rows that changed, and a USB mouse's arrow
  is drawn over the front buffer, so moving it costs no frame.
- **QEMU**: `mini/qemu.c` (ramfb, PCI for the OHCI test rig) and
  `mini/virtio.c` (network, keyboard, tablet).

Everything runs in one loop on one core: poll each device, run the network,
draw a frame if something changed, feed the watchdog, sleep until the next
tick. Nothing waits on anything else. The other three cores wait in a pool
(`mini/arch/cpu.c`) for work that is handed to them: a playing video's
decoding on core 1, and a language model's arithmetic on cores 1 to 3.

- **The SD card**: `mini/sdcard.c` drives MMC0 (4-bit, 50 MHz) and
  `mini/store.c` keeps a small file table past the card's partitions, in
  two copies written in turn so a power cut can't lose it.
- **Packages**: `mini/pkg.c` fetches the list, installs and loads. A
  package (`.bpk`, made by `mini/tools/mkpkg.py`) is position-independent
  code linked at 0, with a list of the places that hold its own addresses
  and a list of the kernel symbols it uses, by name; the loader fills
  those from `mini/exports.c`. `mini/dl.c` does the big downloads
  (redirects, ranges, carrying on after a drop).
- **BoltOS Local AI** is `mini/pkg/localai/`: a GGUF reader, GPT-2's
  byte-level BPE tokenizer (checked token for token against Hugging
  Face's), the Llama/Qwen2/Qwen3 transformer, the core pool glue
  (`engine.c`), Hugging Face downloads (`models.c`) and Exa search
  (`mcp.c`). `kern.c` has NEON kernels for Q4_0, Q4_1, Q8_0 and Q6_K
  weights; at load the weights are rearranged in place so four rows sit
  side by side, which is what lets an in-order A53 keep its multipliers
  busy (0.5 → 1.35 GMAC/s a core). Keys and values are cached as 8-bit
  numbers with scales, so attention runs on the same integer multiplies
  (it costs under 1% in perplexity). The same engine builds on a Mac for
  testing: `cc -O2 -o llmtest test/llmtest.c llm.c tok.c kern.c` in that
  folder (`kern`, `tok`, `chat`, `nll` and `raw` modes).

## Building it

```bash
bash build-mini.sh
```

This uses the same toolchain as the TV edition: clang and ld.lld from
`build/.tools/env` (see TV.md, "Building it"), because clang targets AArch64
without anything extra. The output is `build/mini/boltos.bin` (the kernel),
`build/mini/sdcard.img` (a whole card) and, in `build/mini/pkg/`, each
package's `.bpk` with the `index.json` boltos.dev serves.

To publish a package, put both in the site's R2 bucket (`boltos-packages`,
which `website/worker.js` serves at `/pkg/`):

```bash
cd website
npx wrangler r2 object put boltos-packages/localai-1.0.0.bpk --file ../build/mini/pkg/localai-1.0.0.bpk --remote
npx wrangler r2 object put boltos-packages/index.json --file ../build/mini/pkg/index.json --remote
```

For development, `GET /fs?path=/Code` lists a folder on the card or sends
a file, `POST /fs?path=...` writes one, `/fs?rm=...` removes one and
`/fs?df` says how much room there is. `GET /run?file=/Code/x.py` runs a
program, `/run?status` shows how it's going and what it printed,
`/run?input=...` answers `input()` and `/run?stop` stops it. On the Mac,

```bash
cc -O1 -Imini -o build/pyhost mini/tools/pyhost.c mini/gc.c mini/numfmt.c mini/mathx.c \
   mini/py_*.c mini/cc.c mini/cc_eval.c mini/cc_lib.c
build/pyhost prog.py        # or prog.c, prog.cpp; input lines can follow
```

runs the same interpreters natively, to compare with CPython and clang.
`POST /pkg` on the Pi's web page installs a `.bpk` sent
from the Mac, `GET /pkg` shows what's installed and the store, and
`GET /app?localai&<command>` talks to Local AI (`status`, `load <id>`,
`download <id>`, `ask <question>`, `web auto|always|off`, `new`). U-Boot comes prebuilt in
`mini/firmware/`. `mini/firmware/build-uboot.sh` rebuilds it, and needs
Arm's aarch64 GCC, which the script downloads.
