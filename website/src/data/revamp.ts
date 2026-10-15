// The October 2026 redesign. Every number here comes out of a commit message
// in c8aa34b..442c533 -- measured under QEMU TCG with `display bench`, which
// is the slow case on purpose. Commit subjects are quoted exactly.

export const revamp = {
  dates: 'October 13 – 14, 2026',
  first: '6a34804',
  last: '442c533',
}

// [before, after]: the September desktop against this one, same app, same
// resolution, both maximised where the app allows it.
export type Pair = { before: string; after: string; title: string; note: string }

export const pairs: Pair[] = [
  {
    before: 'v1_desktop',
    after: 'desktop',
    title: 'The desktop',
    note: 'Every app as an icon in 8-pixel type over a grey mountain, and a black bar along the bottom. Now an aurora rendered for each resolution, three panels of glass, and four icons.',
  },
  {
    before: 'v1_startmenu',
    after: 'startmenu',
    title: 'Start',
    note: 'A column of every app at once. Now a pinned grid, an All apps page, and a search box that reaches settings and files.',
  },
  {
    before: 'v1_files_max',
    after: 'files_max',
    title: 'File Explorer',
    note: 'Rebuilt on ui.h, the new control kit: a sidebar of places, a breadcrumb, toolbar glyphs, and a delete button you can actually see.',
  },
  {
    before: 'v1_settings_max',
    after: 'settings_max',
    title: 'Settings',
    note: 'One long page of buttons became pages: Personalization, Display, Sound, Text & fonts, About. Five themes and ten accent colours.',
  },
  {
    before: 'v1_taskmgr_max',
    after: 'taskmgr_max',
    title: 'Task Manager',
    note: 'Rebuilt. It used to open half off a 1024-wide screen.',
  },
  {
    before: 'v1_clock_max',
    after: 'clock_max',
    title: 'Clock',
    note: 'An analogue face drawn with real lines, real numerals and an orange seconds hand. The old one printed the date over its own time.',
  },
  {
    before: 'v1_media_max',
    after: 'media_max',
    title: 'Media',
    note: 'An artwork tile, glyph transport and a mirrored level meter, where there was a row of grey buttons.',
  },
  {
    before: 'v1_sysinfo_max',
    after: 'sysinfo_max',
    title: 'System Info',
    note: 'The same facts, finally with a title that does not sit on its subtitle.',
  },
]

export const numbers = [
  { value: '157→20', unit: 'ms', label: 'to draw a 640×620 glass panel under emulation, once the blur became a copy instead of a stretch' },
  { value: '10,451', unit: '', label: 'lines added across 69 source files, and 3,673 taken out' },
  { value: '23→8', unit: 'ms', label: 'for eighteen dock icons, with atlases drawn at the size the dock asks for' },
  { value: '21', unit: 'h', label: 'from the first glass commit to the rebuilt image, in thirteen commits' },
]

export type Feature = { title: string; body: string; shot?: string; mono: string }

export const features: Feature[] = [
  {
    mono: 'gfx · glass',
    title: 'Liquid glass',
    body: 'The dock, the tray, Start, Quick Settings and the notification centre are frosted glass over whatever is behind them. The blur is captured once, stretched back to full size once, and every frame after that copies rows out of it.',
    shot: 'panel_qs',
  },
  {
    mono: 'gui · damage',
    title: 'A compositor that only redraws what changed',
    body: 'Every window renders into a surface of its own and each frame is built from damage. Windows open, close, minimise and tile with an animation without the app drawing one extra frame, and moving the mouse costs two 32×32 copies instead of a recomposite.',
    shot: 'tiled',
  },
  {
    mono: 'ttf · Inter',
    title: 'Real type',
    body: 'Inter for the interface and DejaVu Sans Mono for the terminal, rasterised from their outlines with exact area coverage, and kerned. Inter is a variable font; the build instances it ahead of time, because applying gvar deltas is not a kernel\'s job.',
    shot: 'terminal_sysinfo',
  },
  {
    mono: 'mkicons.py',
    title: 'Real icons',
    body: 'Every app icon is drawn as SVG and rendered into atlases at 20, 40 and 48 pixels: the sizes the switcher, the dock and Start actually draw them, so nothing is scaled on the way to the screen. The interface glyphs are Lucide\'s.',
    shot: 'panel_start',
  },
  {
    mono: 'shell · tray',
    title: 'Quick Settings and a notification centre',
    body: 'Click the tray for tiles and sliders: network, Focus, dark mode, mute, screen recording, a screenshot, brightness, volume. Click the clock for every notification the machine has posted, with a month calendar underneath.',
    shot: 'panel_nc',
  },
  {
    mono: 'ui.h',
    title: 'Themes, accents and a control kit',
    body: 'Dark, Light, Midnight, Graphite and Sand, and ten accent colours for buttons, selection and the dock\'s active app. Settings, File Explorer and Calculator are rebuilt on ui.h, a small kit of switches, chips and lists the other apps can grow into.',
    shot: 'settings_win',
  },
]

// The apps that were rebuilt rather than just re-typed.
export const rebuilt = [
  { k: 'clock_win', name: 'Clock' },
  { k: 'media_win', name: 'Media' },
  { k: 'calc_win', name: 'Calculator' },
  { k: 'taskmgr_win', name: 'Task Manager' },
  { k: 'mixer_win', name: 'Mixer' },
  { k: 'stopwatch_win', name: 'Stopwatch' },
  { k: 'software_win', name: 'Software' },
  { k: 'files_win', name: 'File Explorer' },
  { k: 'settings_win', name: 'Settings' },
]

// Bugs the redesign walked into. The site likes the bugs that shaped a thing.
export const found = [
  {
    title: 'The heap was sitting on the kernel',
    body: 'The heap was a fixed window at 16 MiB. The new fonts, icons and wallpapers walked the kernel\'s .bss up to meet it, two more icon atlases pushed it over, and the machine reset itself while seeding /bin with nothing on the serial port to say why. The heap now starts wherever the kernel ends.',
    commit: '2a2dfb5',
  },
  {
    title: 'Three icons that were never there',
    body: 'Lucide renamed trash-2, circle-help and smile and kept the old names as one-line re-exports. The icon builder read those as icons, found no shapes and drew empty squares, so File Explorer\'s delete button was invisible, and so was Minesweeper\'s face.',
    commit: '98f385a',
  },
  {
    title: 'Brightness that dimmed one window',
    body: 'The old compositor presented the whole screen every frame; the glass one presents damage. So `display brightness 40` dimmed the Terminal it was typed into and nothing else. A level change now presents the backbuffer once more.',
    commit: '2df9bcd',
  },
  {
    title: 'A shadow that outlived its window\'s focus',
    body: 'A focused window casts a bigger, darker shadow. Losing focus only repainted the title bar, so the old shadow stayed on the wallpaper as a dark band, cut off square where the dock had redrawn above it.',
    commit: '12d9b42',
  },
  {
    title: 'Text placed for an 8-pixel font',
    body: 'Clock printed its date over its own time, Stopwatch\'s digits sat on its buttons, links were underlined through the middle of the letters, and the bottom row of the minefield was under the window\'s edge. Every one of them was code written for the old 8×8 face.',
    commit: 'a4bef11',
  },
  {
    title: 'Animations on a machine that cannot animate',
    body: 'Under QEMU without acceleration a scaled window takes 130 ms to draw, so a 230 ms open animation is two or three frames and a jump. Auto times a frame copy and an alpha fill at boot and turns motion off below the line: 7.1 ms under TCG, 0.3 ms under WHPX.',
    commit: '0be6f2c',
  },
]

export const commits = [
  { h: '6a34804', t: 'Oct 13, 21:46', s: 'gui: a desktop made of glass, with real type and real icons' },
  { h: '4dbac02', t: 'Oct 14, 16:25', s: 'gfx: glass is a copy now, not a stretch every frame' },
  { h: '2a2dfb5', t: 'Oct 14, 17:18', s: 'mem: the heap starts where the kernel ends, not at 16 MiB' },
  { h: '98f385a', t: 'Oct 14, 17:18', s: 'icons: atlases at the sizes the dock draws, and three glyphs that were blank' },
  { h: 'a4bef11', t: 'Oct 14, 17:18', s: 'apps: the older ones, on the new type and without the overlaps' },
  { h: '7fe59da', t: 'Oct 14, 17:24', s: 'shell: a rule across glass is a hairline, not a black line' },
  { h: '0be6f2c', t: 'Oct 14, 17:24', s: 'gui: reduce motion, and Auto turns it on for a slow machine' },
  { h: '2df9bcd', t: 'Oct 14, 17:55', s: 'gui: a brightness change reaches the whole screen again' },
  { h: 'd5348f6', t: 'Oct 14, 17:55', s: 'display: say which wallpaper is on the panel' },
  { h: '87e2358', t: 'Oct 14, 17:55', s: 'test: the GUI tests learn the glass desktop' },
  { h: '12d9b42', t: 'Oct 14, 18:05', s: 'gui: losing focus repaints the shadow, not just the title bar' },
  { h: 'd1505f1', t: 'Oct 14, 18:05', s: 'docs: the desktop as it is now' },
  { h: '442c533', t: 'Oct 14, 18:35', s: 'iso: rebuilt for the finished glass desktop' },
]
