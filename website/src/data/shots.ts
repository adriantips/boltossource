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
  desktop: 'The desktop at 1920×1080, with one wallpaper render per resolution.',
  startmenu: 'The Start menu searches apps first, then your files.',
  bootmenu: 'The boot menu: normal, safe mode, recovery shell, or a boot with fsck.',
  login: 'Signing in. Passwords are salted and stretched with PBKDF2.',
  terminal_sysinfo: 'Terminal: switching the panel to 1080p, then sysinfo.',
  terminal_ps: 'Terminal: the filesystem root and the process list.',
  files: 'File Explorer, in the signed-in user\'s home folder.',
  settings: 'Settings.',
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
  startsearch: 'Start menu search, filtering as you type.',
  doom_play: 'DOOM E1M1, running in a maximised window.',
  strike_play: 'BoltStrike, in a round.',
  media: 'Media, the WAV and MP3 player.',
  music: 'Music.',
  mixer: 'The mixer, with per-channel levels and real mutes.',
  photos: 'Photos.',
  paint: 'Paint.',
  notepad: 'Notepad.',
  sheets: 'Sheets, a spreadsheet.',
  calendar: 'Calendar.',
  calc: 'Calculator.',
  clock: 'Clock.',
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
  stopwatch: 'Stopwatch.',
  doom: 'DOOM, compiled from source into the kernel.',
  gameboy: 'The Game Boy emulator.',
  strike: 'BoltStrike: every texture and sound generated at startup.',
  legacy_desktop: 'June 24, 2026: 1080p on VirtualBox, System Info reading the real CPU.',
  legacy_icons: 'June 28, 2026: the desktop at 1024×768.',
  legacy_login: 'June 2026: the first sign-in screen.',
  legacy_nssite: 'June 24, 2026: an early OldBrowser render.',
  legacy_nsrender: 'June 24, 2026: the NetSurf renderer at work.',
}

export function captionFor(k: string) {
  const base = k.replace(/_(max|win)$/, '')
  const c = captions[k] ?? captions[base] ?? k
  return k.endsWith('_max') && !captions[k] ? c.replace(/\.$/, '') + ', maximised.' : c
}
