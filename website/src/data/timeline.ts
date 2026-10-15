// Dates come from git history and file timestamps in the repo. Commit
// subjects are quoted exactly as they were written.

export type Milestone = {
  date: string
  title: string
  body: string
  commits?: string[]
  shot?: string
  tag?: string
}

export type Era = { name: string; range: string; blurb: string; items: Milestone[] }

export const eras: Era[] = [
  {
    name: 'Before the repo',
    range: 'June 2026',
    blurb: 'A kernel, a desktop and a browser engine, built before any of it was in version control.',
    items: [
      {
        date: 'Jun 19',
        title: 'The first footage',
        body: 'The oldest file in the tree is a screen recording of BoltOS running. The boot chain, the kernel and a framebuffer desktop already existed.',
        tag: 'recording',
      },
      {
        date: 'Jun 23',
        title: 'Real disks',
        body: 'Test images for an HDD, an SSD and a FAT32 volume appear, and with them the ATA driver that tells a spinning disk from a solid-state one by its rotation rate.',
      },
      {
        date: 'Jun 24',
        title: 'A sign-in screen, and 1080p on VirtualBox',
        body: 'The desktop runs at 1920×1080 on VirtualBox\'s display adapter, with System Info reading the real CPU. The same day, the NetSurf-based renderer draws its first pages.',
        shot: 'legacy_desktop',
      },
      {
        date: 'Jun 28',
        title: 'Thirty-two icons',
        body: 'Paint, Sheets, Photos, Music, Calendar, Piano, Minesweeper, 2048 and DOOM are all on the desktop.',
        shot: 'legacy_icons',
      },
      {
        date: 'Jun 30',
        title: 'First push to GitHub',
        body: '"an operating system made completley from scratch in C." The lightning-bolt logo is made the same day.',
        tag: 'github',
      },
    ],
  },
  {
    name: 'Foundations',
    range: 'July 2026',
    blurb: 'The project gets a proper repository and a map of itself.',
    items: [
      {
        date: 'Jul 8',
        title: 'Initial commit',
        body: 'The tree that the rest of this history grows from.',
        commits: ['Initial commit'],
      },
      {
        date: 'Jul 19',
        title: 'Architecture diagrams',
        body: 'Two diagrams of how the pieces fit together, from firmware to apps.',
      },
    ],
  },
  {
    name: 'The sprint',
    range: 'September 7 – 18, 2026',
    blurb: 'One hundred and twenty commits in twelve days. BoltOS goes from a desktop that runs to a system you could live in.',
    items: [
      {
        date: 'Sep 7',
        title: 'Things you can trust',
        body: 'A QEMU regression harness, a boot menu with safe mode, atomic crash-safe filesystem commits, a self-installer, real user accounts, virtio-net, sockets for ring 3 and an SSH-2 server -- in one day.',
        commits: [
          'boot: a boot menu with safe mode, on both legacy and UEFI',
          'fs: atomic crash-safe commits, real flush barriers, and fsck',
          'install: BoltOS can install itself; ring-3 crashes no longer kill the machine',
          'users: real accounts, file ownership enforced in the VFS, sudo and a lock screen',
          'ssh: an SSH-2 server, so the machine can be administered and developed on remotely',
        ],
      },
      {
        date: 'Sep 8',
        title: 'Somewhere to write software',
        body: 'A source-level debugger, IDE projects with targets, local version control, core dumps and a real package manager.',
        commits: [
          'cc: a debugger, so a program that misbehaves can be found out',
          'vcs: a project\'s history, kept on the machine that has the project',
          'pkg: a real package manager -- a format, a repository, dependencies',
        ],
      },
      {
        date: 'Sep 9',
        title: 'Twenty commits',
        body: 'The SDK, a shell you can program in, screenshots, four virtual desktops, rebindable shortcuts, the Software store, HD Audio and WAV playback.',
        commits: [
          'sdk: a program the kernel has never heard of, run anyway',
          'gui: four desktops, and a window that is really on only one',
          'hda: the codec every machine has had for twenty years, actually driven',
        ],
        shot: 'software',
      },
      {
        date: 'Sep 10',
        title: 'v0.1.0, in public',
        body: 'The first public preview is published on GitHub. The same day: an encrypted vault, IPv6 addresses the machine works out for itself, and SSH in the other direction.',
        commits: ['vault: an encrypted directory, where there was a command that XOR\'d a file', 'net: an address the machine works out for itself'],
        tag: 'release',
      },
      {
        date: 'Sep 11',
        title: 'Structs, and two network families',
        body: 'The C compiler learns structs; TCP stops caring whether IPv4 or IPv6 carried the connection.',
        commits: ['cc: structs, so the C compiler is one you can write C in', 'net: a connection that does not care which family carried it'],
      },
      {
        date: 'Sep 12',
        title: 'The machine writes its own programs',
        body: 'An x86-64 assembler checked against NASM, a native compiler backend, a linker, make, and the Wi-Fi stack with WPA2.',
        commits: ['as: the first program on this machine that the machine wrote', 'cc: a second backend, so a program written here becomes one the machine runs', 'ld: a program that no longer has to fit in one file'],
      },
      {
        date: 'Sep 13',
        title: 'Thirty-two commits',
        body: 'Window tiling, file search in the Start menu, 4K scaling, screen recording, microphone capture, USB storage, an MP3 decoder checked against mpg123, and the first fuzzers.',
        commits: ['gui: 8-pixel glyphs on a 4K panel are a joke at the user\'s expense', 'mp3: a Layer III decoder, and mpg123 to say whether it is one', 'fuzz: the two places a stranger picks the bytes, and what was wrong there'],
        shot: 'startmenu',
      },
      {
        date: 'Sep 14',
        title: 'It sleeps',
        body: 'ACPI S3 suspend and resume, and the hard half is coming back.',
        commits: ['s3: it sleeps, and the hard half is coming back'],
      },
      {
        date: 'Sep 15 – 16',
        title: 'Real hardware',
        body: 'One USB image that boots a real stick on BIOS and UEFI. xHCI taken over from the firmware properly. A real keyboard, behind the hub inside it, finally working.',
        commits: ['usb: one image that boots a real stick on BIOS and UEFI both', 'xhci: the reset disabled the port, and the keyboard was behind a hub'],
        tag: 'hardware',
      },
      {
        date: 'Sep 16 – 17',
        title: 'Games',
        body: 'BoltStrike, a Game Boy emulator, and boltcraft on the desktop.',
        commits: ['games: a defusal shooter, and a Game Boy', 'games: boltcraft you can actually reach, and a shooter that stops crawling'],
        shot: 'strike',
      },
      {
        date: 'Sep 18',
        title: 'Eleven things the fuzzers found',
        body: 'The image and audio decoders join the fuzzing harness.',
        commits: ['fuzz: the decoders nobody was fuzzing, and eleven things they got wrong'],
      },
    ],
  },
  {
    name: 'In public',
    range: 'September 2026',
    blurb: 'A website, and a desktop that was about to be replaced.',
    items: [
      {
        date: 'Sep 23',
        title: 'boltos.dev',
        body: 'This site goes up, with every screenshot taken from the build of the day. That desktop is now the Before the revamp set in the gallery.',
        shot: 'v1_desktop',
      },
    ],
  },
  {
    name: 'The huge UI revamp',
    range: 'October 13 – 14, 2026',
    blurb: 'The whole desktop, redrawn in liquid glass: thirteen commits, one evening and a day.',
    items: [
      {
        date: 'Oct 13',
        title: 'A desktop made of glass',
        body: 'A new compositor that builds each frame from damage, and a new shell around it: a floating dock, a tray and a desktop switcher in blurred glass, Start with search, Quick Settings, a notification centre with a calendar, and traffic lights on every window. Text is Inter, rasterised from its outlines.',
        commits: ['gui: a desktop made of glass, with real type and real icons'],
        shot: 'desktop',
        tag: 'redesign',
      },
      {
        date: 'Oct 14',
        title: 'Fast enough to keep, and every app on it',
        body: 'Glass becomes a copy instead of a stretch every frame, 157 ms to 20 under emulation. Icon atlases at the sizes the dock draws. Clock, Media, Mixer, Task Manager, Stopwatch and Software rebuilt; a dozen more fixed. Animations get an Auto that turns them off on a machine too slow to show them.',
        commits: [
          'gfx: glass is a copy now, not a stretch every frame',
          'mem: the heap starts where the kernel ends, not at 16 MiB',
          'apps: the older ones, on the new type and without the overlaps',
          'gui: reduce motion, and Auto turns it on for a slow machine',
          'iso: rebuilt for the finished glass desktop',
        ],
        shot: 'tiled',
      },
    ],
  },
]
