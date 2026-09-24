import { useRef, useState } from 'react'
import { Link } from 'react-router-dom'
import { motion, useMotionTemplate, useScroll, useTransform, useSpring, AnimatePresence } from 'motion/react'
import { ArrowDown, ArrowUpRight, Download as DownloadIcon } from 'lucide-react'
import Lightning from '@/components/bits/Lightning'
import DecryptedText from '@/components/bits/DecryptedText'
import CountUp from '@/components/bits/CountUp'
import ScrollVelocity from '@/components/bits/ScrollVelocity'
import SpotlightCard from '@/components/bits/SpotlightCard'
import { Container, SectionHead, Button, ArrowLink, CodeBlock } from '@/components/site/Blocks'
import { MaskLines, Reveal } from '@/components/site/Reveal'
import { Shot } from '@/components/site/Shot'
import { Mark } from '@/components/site/Logo'
import { systems } from '@/data/systems'
import { stats, downloads } from '@/data/site'
import { hasShot, thumbSrc } from '@/data/shots'

export default function Home() {
  return (
    <>
      <Hero />
      <HeroShot />
      <Marquee />
      <Stats />
      <Layers />
      <Proofs />
      <Showcase />
      <CreatorBand />
      <SprintBand />
      <BootIt />
    </>
  )
}

function Hero() {
  const ref = useRef<HTMLElement>(null)
  const { scrollYProgress } = useScroll({ target: ref, offset: ['start start', 'end start'] })
  // The copy drifts up and out. It used to drift *down*, which walked it
  // past the section's bottom padding into overflow-hidden, and the desktop
  // shot below then sat on top of a sliced-off paragraph.
  const y = useTransform(scrollYProgress, [0, 1], ['0%', '-12%'])
  // Both ranges run to 1 on purpose. motion hands scroll-linked opacity to a
  // native scroll timeline, and a range that stops at 0.06 gets an implicit
  // keyframe at 1 -- the hint faded out and then faded back in.
  const fade = useTransform(scrollYProgress, [0, 0.7, 1], [1, 0, 0])
  const hint = useTransform(scrollYProgress, [0, 0.06, 1], [1, 0, 0])

  return (
    <section ref={ref} className="relative flex min-h-[100svh] items-end overflow-hidden pb-16 sm:pb-20">
      <motion.div style={{ opacity: fade }} className="absolute inset-0 grayscale">
        <div className="absolute inset-0 opacity-75">
          <Lightning hue={220} xOffset={-0.75} speed={0.7} intensity={0.9} size={1.1} />
        </div>
        <div className="absolute inset-0 bg-[radial-gradient(ellipse_at_70%_40%,transparent_0%,#000_70%)]" />
      </motion.div>
      <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-40" />

      <Container className="relative">
        <motion.div style={{ y, opacity: fade }}>
          <motion.div
            initial={{ opacity: 0, y: 12 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ duration: 1, delay: 0.2 }}
            className="mb-8 inline-flex items-center gap-3 rounded-full border border-line bg-black/40 py-1.5 pr-4 pl-1.5 backdrop-blur"
          >
            <span className="rounded-full bg-white px-2.5 py-0.5 font-mono text-[10.5px] font-medium text-black">0.2</span>
            <span className="font-mono text-[11.5px] text-smoke">
              <DecryptedText text="x86-64 · BIOS + UEFI · written from scratch" animateOn="view" sequential speed={28} />
            </span>
          </motion.div>

          <h1 className="display text-[clamp(3.3rem,11.5vw,11.5rem)]">
            <MaskLines lines={['An operating', <>system, from <span className="serif-i">nothing.</span></>]} delay={0.25} />
          </h1>

          <div className="mt-10 grid gap-10 lg:grid-cols-[1.2fr_1fr] lg:items-end">
            <Reveal delay={0.55}>
              <p className="max-w-xl text-[clamp(1.05rem,1.5vw,1.25rem)] leading-relaxed text-smoke">
                BoltOS is a 64-bit kernel, a desktop, a web browser, three compilers and DOOM. About 100,000 lines of C and
                assembly, written by <Link to="/creator" className="text-white underline decoration-white/30 underline-offset-4 hover:decoration-white">Adrian Derakhshideh</Link>, age twelve.
              </p>
            </Reveal>
            <Reveal delay={0.7} className="flex flex-wrap gap-3 lg:justify-end">
              <Button to="/download"><DownloadIcon className="size-4" /> Download BoltOS</Button>
              <Button to="/system" variant="ghost">Explore the system <ArrowUpRight className="size-4" /></Button>
            </Reveal>
          </div>
        </motion.div>
      </Container>

      <motion.div style={{ opacity: hint }} className="absolute bottom-6 left-1/2 hidden -translate-x-1/2 sm:block">
        <motion.div
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          transition={{ delay: 1.6, duration: 1 }}
          className="flex items-center gap-2 font-mono text-[10.5px] tracking-[0.2em] text-fog"
        >
          <ArrowDown className="size-3 animate-bounce" /> SCROLL
        </motion.div>
      </motion.div>
    </section>
  )
}

function HeroShot() {
  const ref = useRef<HTMLDivElement>(null)
  const { scrollYProgress } = useScroll({ target: ref, offset: ['start end', 'center center'] })
  const p = useSpring(scrollYProgress, { stiffness: 90, damping: 24, mass: 0.4 })
  const rotateX = useTransform(p, [0, 1], [28, 0])
  const scale = useTransform(p, [0, 1], [0.84, 1])
  const gray = useTransform(p, [0.55, 1], [1, 0])
  const filter = useMotionTemplate`grayscale(${gray})`

  return (
    <section className="relative pb-24 sm:pb-36" style={{ perspective: 1400 }}>
      <Container>
        <motion.div ref={ref} style={{ rotateX, scale, transformOrigin: 'center top' }} className="relative">
          <div className="absolute -inset-x-10 -top-24 bottom-0 bg-[radial-gradient(ellipse_at_center,rgba(255,255,255,0.14),transparent_65%)] blur-2xl" />
          <div className="relative overflow-hidden rounded-2xl border border-line-strong bg-coal p-1.5 shadow-[0_60px_160px_-40px_rgba(255,255,255,0.18)] sm:rounded-[22px] sm:p-2">
            <motion.img
              src="/shots/desktop.webp"
              alt="The BoltOS desktop at 1920 by 1080"
              width={1920}
              height={1080}
              style={{ filter }}
              className="block h-auto w-full rounded-xl sm:rounded-[16px]"
            />
          </div>
        </motion.div>
        <div className="mt-6 flex flex-col justify-between gap-2 text-[13px] text-fog sm:flex-row">
          <span>The BoltOS desktop, captured from the current build at 1920×1080.</span>
          <span className="font-mono">QEMU · 2 CPUs · 2 GiB</span>
        </div>
      </Container>
    </section>
  )
}

function Marquee() {
  return (
    <section className="border-y border-line py-10 sm:py-14">
      <ScrollVelocity
        texts={[
          <span className="mx-6 inline-flex items-center gap-12 font-medium tracking-[-0.04em]">Boot chain <Star /> Preemptive kernel <Star /> SMP <Star /> BoltFS <Star /> NVMe <Star /> xHCI USB <Star /> TLS 1.3 <Star /> IPv6 <Star /> SSH <Star /></span>,
          <span className="mx-6 inline-flex items-center gap-12 serif-i text-smoke">BoltCC <Star /> BoltJS <Star /> BoltPy <Star /> DOOM <Star /> MP3 <Star /> HD Audio <Star /> PE32+ <Star /> S3 sleep <Star /> Fuzzing <Star /></span>,
        ]}
        velocity={40}
        className="text-[clamp(2.2rem,5.5vw,4.8rem)] leading-[1.15]"
      />
    </section>
  )
}

function Star() {
  return <Mark className="inline size-[0.55em] text-white/40" strokeWidth={4} />
}

function Stats() {
  return (
    <section className="py-24 sm:py-36">
      <Container>
        <div className="grid grid-cols-2 border-t border-l border-line lg:grid-cols-4">
          {stats.map((s, i) => (
            <Reveal key={s.label} delay={i * 0.08} className="border-r border-b border-line p-6 sm:p-10">
              <div className="display text-[clamp(3rem,7vw,6.2rem)] tabular-nums">
                {s.value === 0 ? '0' : <CountUp to={s.value} duration={2.2} separator="," />}
                <span className="text-fog">{s.suffix}</span>
              </div>
              <p className="mt-4 max-w-[16rem] text-[14px] leading-snug text-smoke">{s.label}</p>
            </Reveal>
          ))}
        </div>
      </Container>
    </section>
  )
}

function Layers() {
  const [hover, setHover] = useState<string | null>(null)
  const [pos, setPos] = useState({ x: 0, y: 0 })
  const hovered = systems.find(s => s.slug === hover)

  return (
    <section className="py-12 sm:py-24" onMouseMove={e => setPos({ x: e.clientX, y: e.clientY })}>
      <Container>
        <SectionHead
          eyebrow="Every layer"
          title={<>Fourteen subsystems. <span className="serif-i text-smoke">One author.</span></>}
          intro="There is no Linux underneath, no GRUB in front, no borrowed kernel. Pick a layer and read how it works — each has its own page, with the trade-offs and the bugs that shaped it."
        />
        <div className="mt-16 border-t border-line">
          {systems.map((s, i) => (
            <Reveal key={s.slug} delay={Math.min(i * 0.03, 0.3)} y={16}>
              <Link
                to={`/system/${s.slug}`}
                onMouseEnter={() => setHover(s.slug)}
                onMouseLeave={() => setHover(null)}
                className="group relative grid grid-cols-[3rem_1fr_auto] items-center gap-4 border-b border-line py-6 transition-colors sm:grid-cols-[5rem_1.1fr_1.4fr_auto] sm:py-8"
              >
                <span className="font-mono text-[12px] text-fog transition-colors group-hover:text-white">{s.n}</span>
                <span className="text-[clamp(1.4rem,3vw,2.5rem)] font-medium tracking-[-0.035em] transition-transform duration-700 ease-[cubic-bezier(.16,1,.3,1)] group-hover:translate-x-3">
                  {s.title}
                </span>
                <span className="hidden text-[14.5px] text-fog transition-colors group-hover:text-smoke sm:block">{s.kicker}</span>
                <span className="grid size-10 place-items-center rounded-full border border-line transition-all duration-500 group-hover:border-white group-hover:bg-white group-hover:text-black">
                  <ArrowUpRight className="size-4" />
                </span>
              </Link>
            </Reveal>
          ))}
        </div>
      </Container>

      {/* the screenshot that follows the cursor over the list */}
      <AnimatePresence>
        {hovered && hasShot(hovered.shot) && (
          <motion.div
            key="float"
            className="pointer-events-none fixed z-40 hidden w-[340px] overflow-hidden rounded-xl border border-line-strong shadow-2xl lg:block"
            initial={{ opacity: 0, scale: 0.9 }}
            animate={{ opacity: 1, scale: 1, left: pos.x + 28, top: pos.y - 110 }}
            exit={{ opacity: 0, scale: 0.9 }}
            transition={{ type: 'spring', stiffness: 260, damping: 28, mass: 0.6 }}
          >
            <img src={thumbSrc(hovered.shot)} alt="" className="block w-full grayscale" />
          </motion.div>
        )}
      </AnimatePresence>
    </section>
  )
}

const proofs = [
  { k: 'Boots on BIOS and UEFI', v: 'A 512-byte MBR and a UEFI app written in C, both landing in the same 64-bit kernel.', to: '/system/boot', mono: 'stage1 → stage2 → long mode' },
  { k: 'Survives a pulled plug', v: 'Shadow-slot commits with flush barriers. The next mount finds the old image or the new one, never half of each.', to: '/system/storage', mono: 'fsck · mirrored superblock' },
  { k: 'Speaks TLS 1.3', v: 'X25519, AES-GCM and X.509 chain verification, so HTTPS here resists an active attacker.', to: '/system/network', mono: 'RFC 7748 vectors' },
  { k: 'Writes its own programs', v: 'A C compiler with a native x86-64 backend, an assembler checked against NASM, a linker and make.', to: '/system/toolchain', mono: 'cc -N · as · ld · make' },
  { k: 'Installs itself', v: 'From a running system to a blank disk, bootable both ways, with no second machine.', to: '/system/storage', mono: 'install nvme0n1' },
  { k: 'Runs DOOM', v: 'Compiled from source into the kernel, on a CPU with its FPU switched off.', to: '/system/games', mono: 'E1M1' },
]

function Proofs() {
  return (
    <section className="py-24 sm:py-36">
      <Container>
        <SectionHead eyebrow="Proof, not claims" title={<>Things it <span className="serif-i">actually</span> does.</>} />
        <div className="mt-16 grid gap-3 sm:grid-cols-2 lg:grid-cols-3">
          {proofs.map((p, i) => (
            <Reveal key={p.k} delay={(i % 3) * 0.08}>
              <Link to={p.to} className="block h-full">
                <SpotlightCard className="!rounded-2xl !border-line !bg-ash h-full !p-8" spotlightColor="rgba(255, 255, 255, 0.09)">
                  <p className="font-mono text-[11px] text-fog">{p.mono}</p>
                  <h3 className="mt-12 text-[24px] font-medium tracking-[-0.03em]">{p.k}</h3>
                  <p className="mt-3 text-[14.5px] leading-relaxed text-smoke">{p.v}</p>
                </SpotlightCard>
              </Link>
            </Reveal>
          ))}
        </div>
      </Container>
    </section>
  )
}

const showcase = [
  { shot: 'browser_max', eyebrow: 'Browser', title: 'A layout engine, and a JavaScript engine to drive it.', body: 'Real DOM, CSS cascade with specificity, flexbox and grid, and BoltJS with an event loop, Promises and fetch().', to: '/system/web' },
  { shot: 'files_max', eyebrow: 'Files', title: 'A filesystem that does not tear.', body: 'Owners and permissions checked in one place for the shell, the VFS and the GUI. Commits that survive the power going out.', to: '/system/storage' },
  { shot: 'tiled', eyebrow: 'Desktop', title: 'Snap it, tile it, get on with it.', body: 'Drag a window to an edge or a corner, or walk it there with Win+arrow. Four virtual desktops, and a shell with over 150 commands in one of the quarters.', to: '/system/desktop' },
  { shot: 'ide_max', eyebrow: 'Code', title: 'Write software without leaving it.', body: 'An IDE with projects and syntax highlighting, over a C compiler with a native x86-64 backend, an assembler, a linker and `make`.', to: '/system/toolchain' },
  { shot: 'doom_play', eyebrow: 'Games', title: 'DOOM, with the FPU switched off.', body: 'The real engine, compiled from source into the kernel and driven with integer maths. Next to it: an original shooter, a voxel sandbox and a Game Boy.', to: '/system/games' },
  { shot: 'taskmgr_max', eyebrow: 'Processes', title: 'A crash in ring 3 is a crash in one program.', body: 'Faulting processes are killed, reaped and written up with a symbolised backtrace, and the machine carries on.', to: '/system/kernel' },
]

function Showcase() {
  const items = showcase.filter(s => hasShot(s.shot))
  return (
    <section className="py-12 sm:py-24">
      <Container>
        <SectionHead eyebrow="Inside" title={<>Built to be <span className="serif-i">used.</span></>} intro="Forty-three apps, each one a single C file with draw, key, click and tick callbacks, all running in a compositor that has no GPU to lean on." />
        <div className="mt-20 space-y-28 sm:space-y-40">
          {items.map((s, i) => (
            <div key={s.shot} className="grid items-center gap-10 lg:grid-cols-12 lg:gap-16">
              <Reveal className={i % 2 ? 'lg:order-2 lg:col-span-4' : 'lg:col-span-4'}>
                <p className="eyebrow">{String(i + 1).padStart(2, '0')} — {s.eyebrow}</p>
                <h3 className="mt-5 text-[clamp(1.8rem,3.4vw,2.8rem)] font-medium leading-[1.05] tracking-[-0.04em]">{s.title}</h3>
                <p className="mt-5 text-[16px] leading-relaxed text-smoke">{s.body.split('`').map((t, j) => j % 2 ? <code key={j} className="inline-code">{t}</code> : t)}</p>
                <ArrowLink to={s.to} className="mt-8">Read how it works</ArrowLink>
              </Reveal>
              <Reveal delay={0.1} className={i % 2 ? 'lg:order-1 lg:col-span-8' : 'lg:col-span-8'}>
                <Shot k={s.shot} list={items.map(x => x.shot)} caption={false} />
              </Reveal>
            </div>
          ))}
        </div>
        <div className="mt-24 flex justify-center">
          <Button to="/apps" variant="ghost">See all 43 apps <ArrowUpRight className="size-4" /></Button>
        </div>
      </Container>
    </section>
  )
}

function CreatorBand() {
  return (
    <section className="relative mt-12 overflow-hidden bg-paper text-black sm:mt-20">
      <Container className="py-28 sm:py-40">
        <div className="grid gap-16 lg:grid-cols-[1.5fr_1fr] lg:items-end">
          <div>
            <p className="font-mono text-[11px] tracking-[0.18em] text-black/50 uppercase">The creator</p>
            <h2 className="display mt-8 text-[clamp(2.8rem,8vw,7.5rem)]">
              <MaskLines lines={['Written by a', <span className="serif-i">twelve-year-old.</span>]} />
            </h2>
          </div>
          <Reveal>
            <p className="text-[18px] leading-relaxed text-black/70">
              Adrian Derakhshideh started BoltOS with a boot sector and kept going: a kernel, drivers, a desktop, a browser, compilers,
              a test suite that boots it in a VM sixty-nine different ways. Most people meet these ideas in a university course.
            </p>
            <Link to="/creator" className="group mt-10 inline-flex h-12 items-center gap-2 rounded-full bg-black px-6 text-[14px] font-medium text-white">
              Meet Adrian <ArrowUpRight className="size-4 transition-transform group-hover:translate-x-0.5 group-hover:-translate-y-0.5" />
            </Link>
          </Reveal>
        </div>
      </Container>
    </section>
  )
}

function SprintBand() {
  const bars = [11, 6, 20, 9, 11, 18, 32, 3, 4, 5, 1, 1]
  const max = Math.max(...bars)
  return (
    <section className="py-24 sm:py-36">
      <Container>
        <div className="grid gap-16 lg:grid-cols-[1fr_1.3fr] lg:items-end">
          <div>
            <p className="eyebrow">Timeline</p>
            <h2 className="display mt-6 text-[clamp(2.4rem,5vw,4.5rem)]">120 commits in <span className="serif-i">twelve days.</span></h2>
            <p className="mt-6 max-w-md text-[16px] leading-relaxed text-smoke">
              From September 7 to 18, 2026, BoltOS gained a self-installer, user accounts, an SSH server, a debugger, a package manager,
              an assembler, a linker, an MP3 decoder, suspend-to-RAM and three games.
            </p>
            <ArrowLink to="/timeline" className="mt-8">The full history</ArrowLink>
          </div>
          <Reveal>
            <div className="flex h-64 items-end gap-1.5 border-b border-line sm:gap-3">
              {bars.map((b, i) => (
                <div key={i} className="group relative flex h-full flex-1 flex-col items-center justify-end">
                  <motion.div
                    className="w-full rounded-t-md bg-white/85 group-hover:bg-white"
                    initial={{ height: 0 }}
                    whileInView={{ height: `${(b / max) * 100}%` }}
                    viewport={{ once: true }}
                    transition={{ duration: 1.2, delay: i * 0.05, ease: [0.16, 1, 0.3, 1] }}
                  />
                  <span className="absolute -top-7 font-mono text-[11px] text-fog opacity-0 transition-opacity group-hover:opacity-100">{b}</span>
                </div>
              ))}
            </div>
            <div className="mt-3 flex gap-1.5 font-mono text-[10px] text-fog sm:gap-3">
              {bars.map((_, i) => <span key={i} className="flex-1 text-center">{7 + i}</span>)}
            </div>
            <p className="mt-2 text-right font-mono text-[10.5px] text-fog">commits per day, September 2026</p>
          </Reveal>
        </div>
      </Container>
    </section>
  )
}

function BootIt() {
  const iso = downloads.find(d => d.id === 'vdi')!
  return (
    <section className="relative overflow-hidden border-t border-line py-28 sm:py-40">
      <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-60" />
      <Container className="relative">
        <div className="text-center">
          <Mark className="mx-auto size-16" />
          <h2 className="display mt-10 text-[clamp(3.5rem,12vw,11rem)]">
            <MaskLines lines={[<>Boot <span className="serif-i">it.</span></>]} />
          </h2>
          <p className="mx-auto mt-8 max-w-lg text-[17px] leading-relaxed text-smoke">
            A virtual disk for QEMU and VirtualBox, an image for USB sticks, and the full source. Every file comes straight from GitHub.
          </p>
          <div className="mt-10 flex flex-wrap justify-center gap-3">
            <Button to={iso.href} download><DownloadIcon className="size-4" /> {iso.file} <span className="text-black/50">{iso.size}</span></Button>
            <Button to="/download" variant="ghost">All downloads</Button>
          </div>
        </div>
        <div className="mx-auto mt-20 max-w-2xl">
          <CodeBlock label="run it in QEMU" text={String.raw`qemu-system-x86_64 -m 2G -smp 2 -cpu max \
  -drive file=boltos.vdi,format=vdi -snapshot \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -device intel-hda -device hda-duplex`} />
        </div>
      </Container>
    </section>
  )
}
