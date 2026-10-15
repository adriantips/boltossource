import manifest from './shots.json'

// shots.json is written by scripts/shots.py when the screenshots are
// converted: key -> [width, height]. A key missing from it renders nothing,
// so a screenshot that failed to capture never shows up as a broken image.
const sizes = manifest as unknown as Record<string, [number, number]>

export const hasShot = (k: string) => k in sizes
export const shotSize = (k: string) => ({ w: sizes[k]?.[0] ?? 1920, h: sizes[k]?.[1] ?? 1080 })
export const shotSrc = (k: string) => `/shots/${k}.webp`
export const thumbSrc = (k: string) => `/shots/thumb/${k}.webp`

// The first variant of a capture that exists: e.g. pick('files', '_win', '_max')
// gives the window crop, then the maximised capture, then the plain desktop.
export function pick(k: string, ...suffixes: string[]) {
  for (const s of suffixes) if (hasShot(k + s)) return k + s
  return k
}

const captions: Record<string, string> = {
  desktop: 'The glass desktop at 1920×1080: the dock in the middle, the desktop switcher on the left, the tray on the right.',
  startmenu: 'Start: a pinned grid, All apps, and one box that searches apps, settings and files.',
  quicksettings: 'Quick Settings: network, Focus, dark mode, mute, recording and a screenshot, with brightness and volume underneath.',
  notifications: 'The notification centre, with a month calendar under it.',
  panel_qs: 'Quick Settings, hung off the right edge of the tray.',
  panel_nc: 'The notification centre: everything the machine has posted, and a calendar.',
  panel_start: 'Start, in glass, with icons drawn at the size it shows them.',
  lock: 'The lock screen. Ctrl+Alt+L, or ten idle minutes.',
  bootmenu: 'The boot menu: normal, safe mode, recovery shell, or a boot with fsck.',
  login: 'Signing in. Passwords are salted and stretched with PBKDF2.',
  terminal_sysinfo: 'Terminal: switching the panel to 1080p, then sysinfo.',
  terminal_ps: 'Terminal: the filesystem root and the process list.',
  files: 'File Explorer, in the signed-in user\'s home folder.',
  settings: 'Settings, rebuilt: themes, accent colours and wallpapers.',
  taskmgr: 'Task Manager, showing every process and thread.',
  sysinfo: 'System Info.',
  browser: 'The Browser rendering its welcome page with its own layout engine.',
  ide: 'Code, the IDE, with a project open.',
  software: 'Software, the package manager for people who never open a terminal.',
  security: 'Security: session, firewall and vault.',
  devmgr: 'Device Manager: the PCI bus and the drivers that claimed each device.',
  network: 'Network: the Wi-Fi picker, which says plainly when there is no radio to drive.',
  terminal_net: 'Terminal: netinfo and sysinfo, on a network DHCP configured at boot.',
  tiled: 'Four windows in four quarters, snapped with Win+arrow.',
  startsearch: 'Start search: a best match, then apps, then files.',
  doom_play: 'DOOM E1M1, running in a maximised window.',
  strike_play: 'BoltStrike, in a round.',
  media: 'Media, the WAV and MP3 player, rebuilt around an artwork tile and a level meter.',
  music: 'Music.',
  mixer: 'The mixer, with per-channel levels and real mutes.',
  photos: 'Photos.',
  paint: 'Paint.',
  notepad: 'Notepad.',
  sheets: 'Sheets, a spreadsheet.',
  calendar: 'Calendar.',
  calc: 'Calculator, rebuilt on the new control kit.',
  clock: 'Clock: an analogue face with real numerals and an orange seconds hand.',
  logs: 'Logs: the kernel log, kept across reboots.',
  memory: 'Memory: physical frames, heap and swap.',
  contacts: 'Contacts.',
  tasks: 'Tasks.',
  charmap: 'Character Map.',
  snip: 'Snipping Tool.',
  piano: 'Piano.',
  solitaire: 'Solitaire.',
  mines: 'Minesweeper.',
  '2048': '2048.',
  snake: 'Snake.',
  ttt: 'Tic-Tac-Toe.',
  life: "Conway's Game of Life.",
  matrix: 'Matrix.',
  boltcraft: 'boltcraft\'s launcher window.',
  boltcraft_shot: 'boltcraft: a voxel world built with the public SDK.',
  installer: 'Install BoltOS: pick a disk, see the exact layout, then write it.',
  clipboard: 'Clipboard history.',
  colorpick: 'Color Picker.',
  stopwatch: 'Stopwatch, rebuilt so its digits stay off its buttons.',
  doom: 'DOOM, compiled from source into the kernel.',
  gameboy: 'The Game Boy emulator.',
  strike: 'BoltStrike: every texture and sound generated at startup.',
  legacy_desktop: 'June 24, 2026: 1080p on VirtualBox, System Info reading the real CPU.',
  legacy_icons: 'June 28, 2026: the desktop at 1024×768.',
  legacy_login: 'June 2026: the first sign-in screen.',
  legacy_nssite: 'June 24, 2026: an early OldBrowser render.',
  legacy_nsrender: 'June 24, 2026: the NetSurf renderer at work.',
}

// What the September captures were captioned when they were current, for the
// ones whose caption has since moved on to describe the glass desktop.
const v1Captions: Record<string, string> = {
  desktop: 'The desktop at 1920×1080, with one wallpaper render per resolution.',
  startmenu: 'The Start menu searches apps first, then your files.',
  startsearch: 'Start menu search, filtering as you type.',
  settings: 'Settings.',
  media: 'Media, the WAV and MP3 player.',
  clock: 'Clock.',
  calc: 'Calculator.',
  stopwatch: 'Stopwatch.',
}

export function captionFor(k: string): string {
  // v1_*: the same capture from the September desktop, before the revamp
  if (k.startsWith('v1_')) {
    const key = k.slice(3)
    const old = v1Captions[key.replace(/_(max|win)$/, '')]
    const c = old ? (key.endsWith('_max') ? old.replace(/\.$/, '') + ', maximised.' : old) : captionFor(key)
    return `September 2026, before the revamp. ${c}`
  }
  const base = k.replace(/_(max|win)$/, '')
  const c = captions[k] ?? captions[base] ?? k
  return k.endsWith('_max') && !captions[k] ? c.replace(/\.$/, '') + ', maximised.' : c
}
