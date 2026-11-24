// BoltOS TV: the television edition, October 2026. Everything here is what
// the build in iso/ does today -- the screenshots are QEMU screendumps of it,
// and the limits are written down as plainly as the features.

import { links } from './site'

export const tv = {
  dates: 'October 2026',
  guide: links.blob('TV.md'),
  tree: links.tree('tv'),
}

export const tvNumbers = [
  { value: '0', unit: '', label: 'logins, boot menus or loading screens between power and the picture' },
  { value: '6', unit: 'films', label: 'seamless loops, filmed in 4K, playing as Live TV channels' },
  { value: '293', unit: '', label: 'ad, tracking and smart-TV telemetry networks blocked for every app' },
  { value: '38', unit: 'MB', label: 'for the whole thing, films included, on a USB stick' },
]

export type TvFeature = { title: string; body: string; shot?: string }

export const tvFeatures: TvFeature[] = [
  {
    title: 'Live TV',
    body: 'Six channels of nature, each one a seamless loop cut from a 4K original. Up and down flip channels with a proper banner, number keys tune directly, and OK opens a guide with a still of every channel over the picture, which keeps playing.',
    shot: 'tv_guide',
  },
  {
    title: 'YouTube, played by the TV itself',
    body: 'Search, or pick a topic, and the results come up as a grid of thumbnails. A video plays full screen with its sound, decoded on the set — the H.264 decoder BoltOS Mini uses, and an AAC decoder written for the TV — with no account, no sign-in and no other computer. OK pauses, left and right jump ten seconds, Channel + and − jump a minute.',
  },
  {
    title: 'Netflix, Prime Video, Disney+ and HBO Max',
    body: 'These only play in a browser with a licensed DRM module, which no small operating system can get, so they play in a real Firefox on another computer on your network. The BoltOS Web Engine sends its picture to the TV as H.264, with the sound, and the remote’s pointer, clicks and keys go back the other way. You sign in once, in that Firefox, and it remembers you.',
  },
  {
    title: 'A browser for the sofa',
    body: 'Every page as a reader view: one column of large type, pictures between the paragraphs, links stepped through with the arrows. HTTPS on the kernel’s own TLS 1.3, an on-screen keyboard, and DuckDuckGo search when what you type is not an address.',
    shot: 'tv_web',
  },
  {
    title: 'An ad blocker that is part of the TV',
    body: 'Every name the set looks up is checked against 293 networks — advertising, analytics and the telemetry smart TVs send home — before a packet leaves. The browser also skips ad scripts and closes up the slots they leave. Live counters and a one-press self-test.',
    shot: 'tv_adblock',
  },
  {
    title: 'Apple TV and HDMI',
    body: 'Inputs, as on any television, with a switcher along the bottom. A computer’s HDMI port is an output, so on this hardware they say “No signal” and tell you how to switch with the TV’s own remote — honestly, rather than pretending.',
    shot: 'tv_appletv',
  },
  {
    title: 'Settings that a TV has',
    body: 'Brightness and four picture modes, volume and mute, which film plays at home, network status, where the streaming engine is, a sleep timer that switches the set off, the clock and time zone, updates from boltos.dev that install themselves when the TV starts, and About with every credit.',
    shot: 'tv_settings',
  },
  {
    title: 'Ambient mode',
    body: 'Press Back at home, or leave the remote alone, and everything fades but the film and a small clock. A television that is on and not being used should be a window, not a menu.',
    shot: 'tv_ambient',
  },
]

export const tvLimits = [
  'The HDMI and Apple TV screens cannot show those devices: BoltOS TV runs on a computer, and a computer’s HDMI port sends a picture out, it does not take one in. Use the TV’s Input button for a box plugged into the TV.',
  'Films play at 1080p, decoded in software as MPEG-1, and are silent. The TV scales them to its panel.',
  'Wired Ethernet only (Intel e1000-family, virtio). Sound comes out of the computer’s own jack, not over HDMI.',
  'Settings start from the defaults at every power-on, except automatic updates and the streaming engine’s address, which are kept on the stick. The browser has no JavaScript: text-first sites work, web apps do not.',
  'YouTube plays at 360p: the one stream YouTube gives out with the picture and sound in a single file, which the TV can play without YouTube’s player scripts. Watchable on a big screen, but soft. Live streams don’t play.',
  'Netflix, Prime Video, Disney+ and HBO Max need a second computer: an x86-64 Linux machine running the BoltOS Web Engine, because Google ships Widevine for Firefox on x86-64 Linux only. They come through at the 720p those services give Firefox on Linux.',
]

export const tvSteps = [
  {
    t: 'Get a small PC',
    body: 'Any 64-bit x86 machine: an Intel NUC or N100 mini PC, a small desktop, an old laptop. You cannot install it inside a Samsung, LG or Android TV — those run locked ARM systems — so the PC becomes the TV’s smart part, over HDMI.',
  },
  {
    t: 'Write the stick',
    body: 'Download boltostv-usb.img.gz and write it to any USB stick of 128 MB or more with balenaEtcher, or Rufus in DD mode. Both take the .gz as it is.',
    code: '# macOS / Linux: check the device name twice\ngunzip -c boltostv-usb.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync',
  },
  {
    t: 'Boot it on the TV',
    body: 'Plug the PC into the TV, add a USB keyboard or an air-mouse remote, and Ethernet for YouTube and the browser. Open the boot menu (F12, F11, F7 or Esc on most machines) and pick the stick. On UEFI, switch Secure Boot off first.',
  },
  {
    t: 'Make it part of the set',
    body: 'Put the stick first in the boot order and leave it in. Set the firmware’s “Restore on AC power loss” to On and plug the PC into the TV’s power strip, and switching the TV on switches BoltOS TV on too.',
  },
]

export const tvKeys = [
  ['Arrows', 'Move'],
  ['Enter / OK', 'Select'],
  ['Esc / Backspace', 'Back — on Home, ambient mode'],
  ['Home or Windows key', 'Home, from anywhere'],
  ['Page Up / Down', 'Channel up and down; page through the web; a minute on YouTube'],
  ['0 – 9', 'Tune Live TV'],
  ['+  −  M', 'Volume, mute'],
  ['G  ·  I', 'Guide · channel banner'],
  ['OK  ·  ← →  on YouTube', 'Pause · ten seconds back and on'],
  ['I  ·  Tab  in a streaming app', 'Arrows to the player · type into the page'],
]

export const tvFilms = [
  ['Lofoten Islands', 'Clément Proust'],
  ['Turquoise Coast', 'Kenan Turguç'],
  ['Sundown at Sea', 'Oskar Gross'],
  ['Northern Lights', 'T Honkamies'],
  ['Kauai Coastline', 'Logan Voss'],
  ['Autumn Forest', 'AP Vibes'],
]

export type TvDownload = { id: string; name: string; file: string; size: string; href: string; blurb: string; bestFor: string }

export const tvDownloads: TvDownload[] = [
  {
    id: 'tv-usb',
    name: 'USB stick for your TV',
    file: 'boltostv-usb.img.gz',
    size: '≈ 38 MB',
    href: '/dl/boltostv-usb.img.gz',
    blurb: 'Write it to a USB stick, plug it into a small PC on your TV’s HDMI, and boot. Legacy BIOS and UEFI both work; one copy of the kernel serves both.',
    bestFor: 'A mini PC, NUC or laptop on the TV (balenaEtcher, Rufus in DD mode, dd)',
  },
  {
    id: 'tv-iso',
    name: 'CD image',
    file: 'boltos-tv.iso',
    size: '≈ 39 MB',
    href: '/dl/boltos-tv.iso',
    blurb: 'The same system as a bootable CD image, for trying it in a virtual machine before it goes anywhere near a television.',
    bestFor: 'QEMU, VirtualBox, VMware',
  },
]
