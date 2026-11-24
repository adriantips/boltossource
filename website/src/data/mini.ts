// BoltOS Mini: BoltOS for small ARM boards, October 2026. Everything here is
// what mini/ does today on an Orange Pi Zero3 -- the screenshots come off the
// board itself (/screen.bmp), and the numbers were measured on it.

import { links } from './site'

export const mini = {
  dates: 'October 2026',
  board: 'Orange Pi Zero3',
  guide: links.blob('MINI.md'),
  tree: links.tree('mini'),
}

export const miniNumbers = [
  { value: '0.23', unit: 's', label: 'from U-Boot handing over to the home screen, the SD card’s files mounted' },
  { value: '59', unit: 'fps', label: 'reaching macOS Screen Sharing with the whole screen changing every frame' },
  { value: '0.31', unit: 's', label: 'for fib(25) in Python on the Pi — a quarter of a million calls (0.25 s in C)' },
  { value: '4.2', unit: 'tok/s', label: 'from Qwen3 0.6B on the Pi’s own cores (7.5 from SmolLM2 360M)' },
]

export type MiniFeature = { title: string; body: string; shot?: string }

export const miniFeatures: MiniFeature[] = [
  {
    title: 'A monitor on HDMI, or your Mac',
    body: 'Plug a monitor into the micro-HDMI port and Mini drives it itself — the H618’s display engine, its timing controller and the HDMI PHY, all set up by Mini, because U-Boot has no video — at sizes up to 1080p, picked in Settings → Display. Or use one USB-C cable: it powers the Pi and shows up on the Mac as a network adapter, and the screen opens in macOS’s own Screen Sharing, keyboard and trackpad included, with only the tiles that changed travelling. Both work at once, and so does the keyboard on the Pi’s USB port.',
    shot: 'mini_home',
  },
  {
    title: 'Code: Python, C and C++, run on the Pi',
    body: 'An editor with syntax colours, line numbers, undo, find and replace, auto-indent and the bracket that matches. F5 saves the file and runs it on a spare core, so the screen never waits on it: what it prints streams in below, input() and cin take what you type, Stop ends it, and an error marks its line. Python has big integers, classes, exceptions, comprehensions, f-strings and modules like math, json and collections; C has pointers, structs and malloc in a memory of its own, where a bad pointer is a “Segmentation fault” with its line; C++ adds classes, templates, lambdas and the STL’s strings, vectors, maps and algorithms.',
    shot: 'mini_code',
  },
  {
    title: 'Files on the SD card, readable on a Mac',
    body: 'Code keeps its files in a Code folder on the card’s FAT32 partition, Notes keeps its page there, and Bluetooth pairings, the display size and the theme outlive a power cut in a System folder. Long file names and folders, written so that a power cut mid-save leaves the old file whole. Put the card in a Mac and the same folders are there. The terminal has ls, cd, cat, cp, mv, rm, mkdir and df, and Tab completes a name.',
    shot: 'mini_code_cpp',
  },
  {
    title: 'Real Firefox, on the Pi',
    body: 'The Browser is Firefox, so pages look exactly as Firefox on Windows draws them — React apps, web fonts, video and all. Firefox needs Linux, so Mini runs Linux as a guest on a hypervisor of its own: it keeps the A53’s EL2 for itself, gives the guest three cores and 894 MB, and makes its disks, screen, keyboard, tablet and network card. The first time, the Browser downloads Firefox from boltos.dev onto the card; after that the machine stays up, and coming back to it is instant. Nothing else runs it: no Mac, no second computer.',
  },
  {
    title: 'And a quick browser of its own',
    body: 'Press Q while Firefox starts for BoltOS’s own engine instead — HTML, CSS layout, images and TLS 1.3, the desktop’s page engine — with one address bar: an address opens the page, anything else searches DuckDuckGo, with All, Images and Videos. While you scroll, pictures wait their turn, so the page answers first.',
    shot: 'mini_web',
  },
  {
    title: 'YouTube, decoded on the second core',
    body: 'A video streams as 360p H.264 and plays on an H.264 decoder written for Mini, bit-exact against ffmpeg on YouTube’s streams. It runs alone on the A53’s second core while the first keeps the network and the screen going. No sound yet: Mini has no audio output.',
    shot: 'mini_video',
  },
  {
    title: 'Packages, from boltos.dev',
    body: 'Apps that aren’t in the kernel come from the Packages app: it lists what boltos.dev offers, downloads one over Ethernet, checks its SHA-256, keeps it on the SD card past the boot partition and loads it — linked against the kernel by name, like a shared library. It joins the bar, and comes back at every start.',
    shot: 'mini_packages',
  },
  {
    title: 'Local AI, on the Pi’s own cores',
    body: 'BoltOS Local AI is the first package: a chat with a language model that runs on the board itself. Pick Qwen3 0.6B, Qwen2.5 0.5B or SmolLM2 360M and the GGUF file downloads straight from Hugging Face onto the card; a transformer written for Mini runs it on three of the A53s with NEON, while the first core keeps the screen going. Questions about today, the news, the weather or prices search the web first through Exa’s MCP server, and the answer lists its sources.',
    shot: 'mini_ai',
  },
  {
    title: 'A terminal and a monitor',
    body: 'A small shell — the SD card’s files, plus about, net, usb, mem, cpu, log, time, theme and reboot; edit and run hand a file to Code — and a Monitor with two minutes of processor load, memory, the link to the Mac and what is on the USB ports.',
    shot: 'mini_terminal',
  },
  {
    title: 'Bluetooth on the board’s own chip, and Ethernet',
    body: 'Bluetooth finds and pairs keyboards, mice and game controllers, classic and LE, on the Zero3’s UWE5622. The Pi goes online over its Ethernet socket, and Settings → Network shows the cable’s speed, the address the router gave, the router and DNS. The display size, themes, accent colour, the screen-sharing password, date and time, updates, restart and power off live in Settings too.',
    shot: 'mini_settings',
  },
  {
    title: 'It keeps itself up to date',
    body: 'Mini asks boltos.dev for a newer version when it starts and every few hours. A quick update downloads, goes onto the SD card beside the old kernel and the Pi restarts into it before you have opened anything; a big one waits in Settings → Updates for you to press Update. The switch turns the automatic part off.',
    shot: 'mini_updates',
  },
  {
    title: 'Flat, and quick because of it',
    body: 'A bar along the top with the apps by number, the network, the load and the clock; solid colours and hairlines underneath, in Dark, Light or Black. No blur, no shadows: a frame takes one to three milliseconds to draw, and a screen that is mostly one colour is what crosses the cable fastest.',
    shot: 'mini_home_light',
  },
]

export const miniSteps = [
  {
    t: 'Get the board and a cable',
    body: 'An Orange Pi Zero3 (Mini is made on the 1 GB one) and a microSD card of any size. For the screen, either a monitor with a micro-HDMI to HDMI cable, or a Mac and a USB-A to USB-C cable that carries data, not only power — on the Mac’s side it goes into a USB-A port; on Apple’s USB-C adapter, the rectangular one.',
  },
  {
    t: 'Write the card',
    body: 'Download boltos-mini-sdcard.img.gz and write it with balenaEtcher, which takes the .gz as it is, or from the terminal. Mini uses the first 130 MB of the card.',
    code: '# macOS: find the card with  diskutil list external,  then\ndiskutil unmountDisk disk4\ngunzip -c boltos-mini-sdcard.img.gz | sudo dd of=/dev/rdisk4 bs=4m',
  },
  {
    t: 'Plug it into the Mac',
    body: 'Card in the Pi, the cable’s USB-C end in the Pi’s USB-C socket. Click Allow if macOS asks about an accessory. A few seconds later a network adapter called BoltOS Mini appears; the Pi is 10.77.0.1. With a monitor instead, plug it into the micro-HDMI port, a keyboard into USB-A and a USB power supply into USB-C.',
  },
  {
    t: 'Open the screen',
    body: 'On a monitor it is already there. On the Mac, in Finder choose Go → Connect to Server (⌘K), type vnc://10.77.0.1 and use the password boltos. Plug in Ethernet for the browser, and its clock sets itself from the network.',
  },
]

export const miniKeys = [
  ['← →  and Return', 'Choose an app on Home, open it'],
  ['1 – 9  or  F1 – F9', 'Open an app directly'],
  ['F5  ·  Ctrl+S  in Code', 'Save and run · save (⌘ works too)'],
  ['Ctrl+F  ·  Ctrl+/  ·  Ctrl+Z', 'Find and replace · comment lines · undo'],
  ['Esc  or  ⌘ / Windows', 'Back to Home (Esc twice from Firefox)'],
  ['↑ ↓ ← →  in Settings', 'Move, change a value'],
  ['Space  ·  ← →  on a video', 'Pause and play · ten seconds back and on'],
  ['Space  ·  R  in Clock', 'Start and stop · reset the stopwatch'],
]

export const miniLimits = [
  'HDMI carries the picture only, not sound, and sizes stop at 1920 × 1200, so a 4K monitor gets its largest mode under that. A small SPI panel on the pin header would need a driver of its own.',
  'No sound. Videos play silently, in Firefox too, and Bluetooth headphones and speakers can’t be used.',
  'Firefox is a download of about 310 MB the first time, and its first start, read off the card, takes the better part of a minute. It has no settings page of its own yet.',
  'Wi-Fi is switched off for now, so the Pi goes online over Ethernet. The image here has no Bluetooth: its firmware is Unisoc’s, not ours to hand out; build from source and the build fetches it from Orange Pi. Bluetooth pairing is new and has met few devices.',
  'Code’s Python, C and C++ are interpreters written for Mini, not CPython or a compiler: most programs a course or a book has run as they would, but Python has no generators (yield) yet, C has no goto, and C++ has no stringstream or user-defined template specializations.',
  'The language models are small ones — what fits in 1 GB at a readable speed. They answer everyday questions well and get facts wrong sometimes; web search helps with anything recent.',
]

export type MiniDownload = { id: string; name: string; file: string; size: string; href: string; blurb: string; bestFor: string }

export const miniDownloads: MiniDownload[] = [
  {
    id: 'mini-card',
    name: 'microSD card for the Zero3',
    file: 'boltos-mini-sdcard.img.gz',
    size: '≈ 1.7 MB',
    href: '/dl/boltos-mini-sdcard.img.gz',
    blurb: 'The whole card: U-Boot and BoltOS Mini, ready for an HDMI monitor or the Mac’s Screen Sharing. Without the Bluetooth firmware, which isn’t ours to hand out — Ethernet and the USB-C link to the Mac work as they are.',
    bestFor: 'An Orange Pi Zero3 and a Mac (balenaEtcher, dd)',
  },
  {
    id: 'mini-src',
    name: 'Build it yourself',
    file: 'bash build-mini.sh',
    size: 'source',
    href: links.tree('mini'),
    blurb: 'clang and ld.lld build the kernel and the card on macOS or Linux, fetching the Bluetooth firmware from Orange Pi at a fixed commit. After the first card, new kernels go over the USB-C cable with pi.py.',
    bestFor: 'Bluetooth, and changing things',
  },
]
