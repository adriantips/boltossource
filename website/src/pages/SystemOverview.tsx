import { Link } from 'react-router-dom'
import { ArrowUpRight } from 'lucide-react'
import { Container, PageHero, SectionHead, CodeBlock } from '@/components/site/Blocks'
import { Reveal } from '@/components/site/Reveal'
import { systems } from '@/data/systems'
import { hasShot, thumbSrc } from '@/data/shots'

// The stack, top to bottom. Each chip links to the page that explains it.
const stack: { layer: string; ring: string; chips: [string, string][] }[] = [
  { layer: 'Applications', ring: 'ring 3 / desktop', chips: [['43 desktop apps', 'desktop'], ['Browser + BoltJS', 'web'], ['Code IDE', 'toolchain'], ['DOOM · BoltStrike · boltcraft', 'games'], ['Packages', 'software']] },
  { layer: 'Desktop', ring: 'compositor', chips: [['Windows, tiling, 4 desktops', 'desktop'], ['Start menu search', 'desktop'], ['TrueType', 'desktop'], ['Screenshots + recording', 'desktop']] },
  { layer: 'Services', ring: 'ring 0', chips: [['TCP/IP · TLS 1.3 · SSH', 'network'], ['BoltFS · FAT32 · EXT2', 'storage'], ['Accounts · vault', 'security'], ['Audio · MP3', 'media'], ['BoltCC · as · ld', 'toolchain']] },
  { layer: 'Kernel', ring: 'ring 0', chips: [['Scheduler + processes', 'kernel'], ['Syscalls', 'kernel'], ['Paging · heap · swap', 'memory'], ['ACPI · APIC · SMP · S3', 'kernel']] },
  { layer: 'Drivers', ring: 'ring 0', chips: [['xHCI + HID + hubs', 'hardware'], ['NVMe · AHCI · ATA', 'storage'], ['e1000 · virtio-net', 'network'], ['HDA · AC\'97', 'media'], ['GPU / DISPI', 'hardware']] },
  { layer: 'Boot', ring: 'real → long mode', chips: [['stage1 MBR', 'boot'], ['stage2', 'boot'], ['BOOTX64.EFI', 'boot'], ['Boot menu', 'boot']] },
]

export default function SystemOverview() {
  return (
    <>
      <PageHero
        eyebrow="Architecture"
        title={['Every layer,', <span className="serif-i text-smoke">from the metal up.</span>]}
        intro="BoltOS is one person's answer to every question an operating system has to answer, from which instruction runs first after power-on to how a click becomes a window moving. This is the map."
      />

      <section className="pb-24 sm:pb-36">
        <Container>
          <div className="overflow-hidden rounded-3xl border border-line">
            {stack.map((row, i) => (
              <Reveal key={row.layer} delay={i * 0.06} y={14}>
                <div className="grid gap-4 border-b border-line p-5 last:border-b-0 sm:grid-cols-[14rem_1fr] sm:items-center sm:p-7" style={{ background: `rgba(255,255,255,${0.012 + (stack.length - i) * 0.006})` }}>
                  <div>
                    <p className="text-[20px] font-medium tracking-[-0.03em]">{row.layer}</p>
                    <p className="mt-1 font-mono text-[11px] text-fog">{row.ring}</p>
                  </div>
                  <div className="flex flex-wrap gap-2">
                    {row.chips.map(([label, slug]) => (
                      <Link key={label} to={`/system/${slug}`} className="rounded-full border border-line bg-black/40 px-3.5 py-1.5 text-[13px] text-smoke transition-colors hover:border-white hover:text-white">
                        {label}
                      </Link>
                    ))}
                  </div>
                </div>
              </Reveal>
            ))}
            <div className="flex items-center justify-between bg-white px-5 py-4 text-black sm:px-7">
              <span className="text-[15px] font-medium">Hardware</span>
              <span className="font-mono text-[11px] text-black/60">x86-64 · real machines, QEMU, VirtualBox, VMware</span>
            </div>
          </div>
        </Container>
      </section>

      <section className="pb-24 sm:pb-36">
        <Container>
          <SectionHead eyebrow="The subsystems" title={<>Fourteen deep dives.</>} intro="Each page covers what the subsystem does, how it does it, and the specific bugs and trade-offs that made it the shape it is." />
          <div className="mt-16 grid gap-px overflow-hidden rounded-3xl border border-line bg-line sm:grid-cols-2 lg:grid-cols-3">
            {systems.map((s, i) => (
              <Reveal key={s.slug} delay={(i % 3) * 0.06} className="bg-black">
                <Link to={`/system/${s.slug}`} className="group flex h-full flex-col p-6 sm:p-8">
                  <div className="relative aspect-[16/9] overflow-hidden rounded-xl border border-line bg-coal">
                    {hasShot(s.shot) && <img src={thumbSrc(s.shot)} alt="" loading="lazy" className="mono-img size-full object-cover group-hover:scale-[1.04]" />}
                  </div>
                  <div className="mt-7 flex items-baseline justify-between gap-4">
                    <h3 className="text-[22px] font-medium tracking-[-0.03em]">{s.title}</h3>
                    <span className="font-mono text-[11px] text-fog">{s.n}</span>
                  </div>
                  <p className="mt-2 flex-1 text-[14.5px] leading-relaxed text-smoke">{s.kicker}</p>
                  <span className="mt-6 inline-flex items-center gap-1.5 text-[13px] text-fog transition-colors group-hover:text-white">
                    Read <ArrowUpRight className="size-3.5" />
                  </span>
                </Link>
              </Reveal>
            ))}
          </div>
        </Container>
      </section>

      <section className="border-t border-line py-24 sm:py-36">
        <Container>
          <div className="grid gap-12 lg:grid-cols-2 lg:gap-20">
            <div>
              <SectionHead eyebrow="Build it yourself" title="Two commands." className="!grid-cols-1 !gap-6" />
              <Reveal delay={0.1}>
                <p className="mt-6 max-w-lg text-[16px] leading-relaxed text-smoke">
                  The build is one script with no incremental logic: every source is compiled every time, so a plain build is always a clean one.
                  It produces the raw image, the bootable ISO, the UEFI loader, the USB image and the embedded userland programs, and
                  compiles DOOM from source into the kernel along the way. About four minutes with clang and lld from msys2.
                </p>
              </Reveal>
            </div>
            <Reveal delay={0.15}>
              <CodeBlock label="msys2 / linux" text={'bash build.sh        # -> iso/os.img, iso/boltos.iso, iso/boltusb.img\nbash run.sh          # boot in QEMU, kernel log on serial\nbash run-uefi.sh     # the same, through OVMF\n\npython test/run_all.py   # the whole suite, ~40 minutes'} />
            </Reveal>
          </div>
        </Container>
      </section>
    </>
  )
}
