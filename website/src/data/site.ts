// Every outside link on the site lives here, so moving the repo or cutting a
// new release is a one-file change. Nothing is hosted by the website itself:
// images are screenshots in /public, every download goes straight to GitHub.

export const REPO = 'https://github.com/adriantips/boltossource'
export const RAW = `${REPO}/raw/main`
export const RELEASES = `${REPO}/releases`

export const links = {
  repo: REPO,
  releases: RELEASES,
  issues: `${REPO}/issues`,
  commits: `${REPO}/commits/main`,
  readme: `${REPO}#readme`,
  guide: `${REPO}/blob/main/GUIDE.md`,
  developing: `${REPO}/blob/main/DEVELOPING.md`,
  sdk: `${REPO}/blob/main/SDK.md`,
  shell: `${REPO}/blob/main/SHELL.md`,
  roadmap: `${REPO}/blob/main/ROADMAP.md`,
  tree: (path: string) => `${REPO}/tree/main/${path}`,
  blob: (path: string) => `${REPO}/blob/main/${path}`,
}

export type Download = {
  id: string
  name: string
  file: string
  size: string
  href: string
  kind: 'image' | 'source'
  blurb: string
  bestFor: string
}

export const downloads: Download[] = [
  {
    id: 'vdi',
    name: 'Virtual disk',
    file: 'boltos.vdi',
    size: '≈ 14 MB',
    href: `${RAW}/iso/boltos.vdi`,
    kind: 'image',
    blurb: 'A ready-made hard disk. Attach it to a new 64-bit VM and power on — it boots straight to the sign-in screen.',
    bestFor: 'QEMU, VirtualBox',
  },
  {
    id: 'img',
    name: 'USB disk image',
    file: 'boltusb.img.gz',
    size: '≈ 14 MB',
    href: `${RAW}/iso/boltusb.img.gz`,
    kind: 'image',
    blurb: 'A raw disk image for a USB stick, with an MBR boot path and an EFI system partition, gzipped. Rufus and balenaEtcher write the .gz as it is. Boots real hardware on BIOS and UEFI.',
    bestFor: 'Real PCs (Rufus in DD mode, balenaEtcher, dd) and QEMU',
  },
  {
    id: 'iso',
    name: 'CD image',
    file: 'boltos.iso',
    size: '≈ 12 MB',
    href: `${RAW}/iso/boltos.iso`,
    kind: 'image',
    blurb: 'The same system as a bootable CD (El Torito, hard-disk emulation). Boot it as a CD-ROM in any VM and it goes straight to the sign-in screen.',
    bestFor: 'QEMU, VirtualBox, VMware as a CD-ROM',
  },
  {
    id: 'zip',
    name: 'Source code',
    file: 'boltossource-main.zip',
    size: 'zip',
    href: `${REPO}/archive/refs/heads/main.zip`,
    kind: 'source',
    blurb: 'The whole tree: kernel, drivers, boot loaders, apps, tests and build scripts.',
    bestFor: 'Windows, or anyone without git',
  },
  {
    id: 'tar',
    name: 'Source code',
    file: 'boltossource-main.tar.gz',
    size: 'tar.gz',
    href: `${REPO}/archive/refs/heads/main.tar.gz`,
    kind: 'source',
    blurb: 'The same tree as a gzipped tarball.',
    bestFor: 'Linux, macOS, msys2',
  },
]

export const stats = [
  { value: 100, suffix: 'k', label: 'lines of first-party C and assembly' },
  { value: 43, suffix: '', label: 'desktop apps, every one written for it' },
  { value: 69, suffix: '', label: 'QEMU tests that boot the real image' },
  { value: 0, suffix: '', label: 'lines of Linux, GRUB or Multiboot' },
]
