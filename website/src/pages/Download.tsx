import { ArrowDownToLine, ArrowUpRight, Disc3, FileArchive, HardDrive, Usb } from 'lucide-react'
import { Container, PageHero, SectionHead, CodeBlock, ArrowLink } from '@/components/site/Blocks'
import { Reveal } from '@/components/site/Reveal'
import { Shot } from '@/components/site/Shot'
import { GitHubIcon } from '@/components/site/GitHubIcon'
import { downloads, links } from '@/data/site'

const icons = { iso: Disc3, img: Usb, vdi: HardDrive, zip: FileArchive, tar: FileArchive } as const

const requirements = [
  ['CPU', 'x86-64, one core or more'],
  ['Memory', '512 MiB to boot, 2 GiB to be comfortable'],
  ['Disk', '512 MiB for a minimal install'],
  ['Firmware', 'Legacy BIOS or UEFI — both work'],
  ['Display', 'Anything the firmware can give a linear framebuffer'],
  ['Network', 'Intel e1000, or virtio-net'],
]

const ways = [
  {
    t: 'QEMU',
    body: 'The quickest way to meet it. Two CPUs, 2 GiB, an e1000 for the network and HD Audio for sound. -snapshot keeps the disk image untouched, so every boot starts fresh.',
    code: String.raw`qemu-system-x86_64 -m 2G -smp 2 -cpu max \
  -drive file=boltos.vdi,format=vdi -snapshot \
  -vga std -netdev user,id=n0 -device e1000,netdev=n0 \
  -device intel-hda -device hda-duplex`,
  },
  {
    t: 'VirtualBox',
    body: 'Type: Other, Version: Other/Unknown (64-bit), BIOS firmware, 2 GiB. Attach boltos.vdi as the hard disk on an IDE controller and set the network adapter to Intel PRO/1000 MT Desktop (82540EM).',
    code: String.raw`VBoxManage createvm --name BoltOS --ostype Other_64 --register
VBoxManage modifyvm BoltOS --memory 2048 --cpus 2 --ioapic on \
  --firmware bios --graphicscontroller vboxvga --vram 128 \
  --nic1 nat --nictype1 82540EM
VBoxManage storagectl BoltOS --name IDE --add ide --controller PIIX4
VBoxManage storageattach BoltOS --storagectl IDE --port 0 --device 0 \
  --type hdd --medium boltos.vdi
VBoxManage startvm BoltOS`,
  },
  {
    t: 'A real PC',
    body: 'Write boltusb.img.gz to a USB stick with Rufus (DD mode) or balenaEtcher, both of which unpack it on the way, or with dd. It boots on legacy BIOS and on UEFI. Installing writes over the whole target disk, so pick one you are willing to lose.',
    code: '# Linux / macOS: check the device name twice\ngunzip -c boltusb.img.gz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync',
  },
]

export default function Download() {
  const images = downloads.filter(d => d.kind === 'image')
  const source = downloads.filter(d => d.kind === 'source')

  return (
    <>
      <PageHero
        eyebrow="Download · version 0.2"
        title={['Download', <span className="serif-i text-smoke">BoltOS.</span>]}
        intro="Every file below is served straight from GitHub, so what you download is exactly what is in the repository. Sign in with user / bolt."
      />

      <Container className="pb-24">
        <div className="grid gap-4 lg:grid-cols-3">
          {images.map((d, i) => {
            const Icon = icons[d.id as keyof typeof icons]
            const primary = i === 0
            return (
              <Reveal key={d.id} delay={i * 0.08}>
                <a
                  href={d.href}
                  className={
                    'group flex h-full flex-col rounded-3xl border p-7 transition-all duration-500 sm:p-9 ' +
                    (primary ? 'border-white bg-white text-black hover:bg-white/90' : 'border-line bg-ash hover:border-white/40')
                  }
                >
                  <div className="flex items-center justify-between">
                    <Icon className="size-7" strokeWidth={1.4} />
                    <span className={'font-mono text-[11px] ' + (primary ? 'text-black/50' : 'text-fog')}>{d.size}</span>
                  </div>
                  <h3 className="mt-16 text-[28px] font-medium tracking-[-0.035em]">{d.name}</h3>
                  <p className={'mt-1 font-mono text-[13px] ' + (primary ? 'text-black/60' : 'text-fog')}>{d.file}</p>
                  <p className={'mt-5 flex-1 text-[15px] leading-relaxed ' + (primary ? 'text-black/70' : 'text-smoke')}>{d.blurb}</p>
                  <p className={'mt-6 text-[12.5px] ' + (primary ? 'text-black/50' : 'text-fog')}>Best for: {d.bestFor}</p>
                  <span className={'mt-8 inline-flex h-12 items-center justify-center gap-2 rounded-full text-[14px] font-medium ' + (primary ? 'bg-black text-white' : 'border border-line-strong group-hover:bg-white group-hover:text-black')}>
                    <ArrowDownToLine className="size-4" /> Download
                  </span>
                </a>
              </Reveal>
            )
          })}
        </div>

        <Reveal className="mt-4">
          <div className="grid gap-6 rounded-3xl border border-line p-7 sm:p-9 lg:grid-cols-[1fr_auto] lg:items-center">
            <div className="flex items-start gap-5">
              <GitHubIcon className="mt-1 size-7 shrink-0" />
              <div>
                <h3 className="text-[24px] font-medium tracking-[-0.03em]">Source code</h3>
                <p className="mt-2 max-w-xl text-[15px] leading-relaxed text-smoke">
                  The whole tree — boot loaders, kernel, drivers, apps, SDK and tests — as GitHub builds it from the <code className="inline-code">main</code> branch.
                  Or clone it: <code className="inline-code">git clone {links.repo}.git</code>
                </p>
              </div>
            </div>
            <div className="flex flex-wrap gap-2">
              {source.map(d => (
                <a key={d.id} href={d.href} className="inline-flex h-12 items-center gap-2 rounded-full border border-line-strong px-5 text-[14px] transition-colors hover:bg-white hover:text-black">
                  <ArrowDownToLine className="size-4" /> .{d.size}
                </a>
              ))}
              <a href={links.repo} target="_blank" rel="noreferrer" className="inline-flex h-12 items-center gap-2 rounded-full bg-white px-5 text-[14px] font-medium text-black">
                Repository <ArrowUpRight className="size-4" />
              </a>
            </div>
          </div>
        </Reveal>
        <p className="mt-5 text-[13px] text-fog">
          Looking for an older build? Every tagged version is on the <a href={links.releases} target="_blank" rel="noreferrer" className="text-smoke underline decoration-white/30 underline-offset-4 hover:text-white">releases page</a>.
        </p>
      </Container>

      <section className="border-t border-line py-24 sm:py-32">
        <Container>
          <SectionHead eyebrow="Run it" title={<>Three ways <span className="serif-i text-smoke">in.</span></>} intro="Try it in a virtual machine first. Everything works from the live image except keeping your files across a reboot — that needs the installer." />
          <div className="mt-16 space-y-4">
            {ways.map((w, i) => (
              <Reveal key={w.t} delay={i * 0.06}>
                <div className="grid gap-8 rounded-3xl border border-line p-7 sm:p-9 lg:grid-cols-[1fr_1.4fr]">
                  <div>
                    <p className="font-mono text-[11px] text-fog">0{i + 1}</p>
                    <h3 className="mt-4 text-[28px] font-medium tracking-[-0.035em]">{w.t}</h3>
                    <p className="mt-3 text-[15px] leading-relaxed text-smoke">{w.body}</p>
                  </div>
                  <CodeBlock text={w.code} label={w.t.toLowerCase()} />
                </div>
              </Reveal>
            ))}
          </div>
        </Container>
      </section>

      <section className="border-t border-line py-24 sm:py-32">
        <Container className="grid gap-16 lg:grid-cols-2">
          <div>
            <SectionHead eyebrow="Requirements" title="What it needs." className="!grid-cols-1 !gap-6" />
            <dl className="mt-10 divide-y divide-line border-y border-line">
              {requirements.map(([k, v]) => (
                <div key={k} className="grid grid-cols-[8rem_1fr] gap-4 py-4">
                  <dt className="text-[14px] text-fog">{k}</dt>
                  <dd className="text-[15px]">{v}</dd>
                </div>
              ))}
            </dl>
            <p className="mt-8 text-[14.5px] leading-relaxed text-smoke">
              Within five seconds of power-on you will see the boot menu. Press <strong className="font-medium text-white">1</strong>, or wait.
              If something stops the desktop from starting, <strong className="font-medium text-white">2</strong> is safe mode.
            </p>
            <ArrowLink to={links.guide} className="mt-8">The full user guide</ArrowLink>
          </div>
          <Reveal>
            <Shot k="bootmenu" />
          </Reveal>
        </Container>
      </section>
    </>
  )
}
