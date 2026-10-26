import { useRef } from 'react'
import { motion, useScroll, useSpring, useTransform } from 'motion/react'
import { ArrowDown, Download as DownloadIcon, Tv as TvIcon, Usb, Disc3 } from 'lucide-react'
import { Container, Button, CodeBlock, SectionHead } from '@/components/site/Blocks'
import { MaskLines, Reveal } from '@/components/site/Reveal'
import { Shot } from '@/components/site/Shot'
import { hasShot } from '@/data/shots'
import { tv, tvNumbers, tvFeatures, tvLimits, tvSteps, tvKeys, tvFilms, tvDownloads } from '@/data/tv'
import { cn } from '@/lib/utils'

export default function Tv() {
  return (
    <>
      <Hero />
      <Numbers />
      <Features />
      <Install />
      <Remote />
      <Limits />
      <Credits />
      <Cta />
    </>
  )
}

function Hero() {
  const ref = useRef<HTMLDivElement>(null)
  const { scrollYProgress } = useScroll({ target: ref, offset: ['start end', 'center center'] })
  const p = useSpring(scrollYProgress, { stiffness: 90, damping: 24, mass: 0.4 })
  const rotateX = useTransform(p, [0, 1], [24, 0])
  const scale = useTransform(p, [0, 1], [0.88, 1])
  const usb = tvDownloads[0]

  return (
    <section className="relative overflow-hidden pt-36 pb-24 sm:pt-44 sm:pb-36">
      <div className="aurora-field pointer-events-none absolute -inset-x-20 -top-20 h-[900px] opacity-90" />
      <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-40" />
      <Container className="relative">
        <Reveal>
          <p className="inline-flex items-center gap-3 rounded-full border border-white/15 bg-black/40 py-1.5 pr-4 pl-1.5 backdrop-blur">
            <span className="aurora-chip rounded-full px-2.5 py-0.5 font-mono text-[10.5px] font-medium text-black">NEW</span>
            <span className="font-mono text-[11.5px] text-smoke">{tv.dates} · the television edition</span>
          </p>
        </Reveal>
        <h1 className="display mt-8 text-[clamp(4.2rem,16.5vw,17rem)] !leading-[0.86]">
          <MaskLines lines={['BoltOS', <span className="aurora-text">TV.</span>]} delay={0.1} />
        </h1>
        <div className="mt-10 grid gap-10 lg:grid-cols-[1.3fr_1fr] lg:items-end">
          <Reveal delay={0.35}>
            <p className="max-w-2xl text-[clamp(1.1rem,1.7vw,1.4rem)] leading-relaxed text-white/80">
              Switch it on and a film fills the screen. No login, no boot menu, no loading bar — just a slim pane of liquid glass
              with your apps: Live TV, Apple TV, HDMI, a browser for the sofa and an ad blocker for the whole set. The same kernel
              as the desktop, with the desktop taken away.
            </p>
          </Reveal>
          <Reveal delay={0.5} className="flex flex-wrap gap-3 lg:justify-end">
            <Button to={usb.href} download><DownloadIcon className="size-4" /> Get it for your TV</Button>
            <a href="#install" className="inline-flex h-12 items-center gap-2 rounded-full border border-line-strong px-6 text-[14px] font-medium text-white transition-colors hover:border-white hover:bg-white/5">
              How to install <ArrowDown className="size-4" />
            </a>
          </Reveal>
        </div>
      </Container>

      <div className="relative mt-20 sm:mt-28" style={{ perspective: 1400 }}>
        <Container>
          <motion.div ref={ref} style={{ rotateX, scale, transformOrigin: 'center top' }} className="relative">
            <div className="aurora-field absolute -inset-10 opacity-70" />
            <div className="relative overflow-hidden rounded-[18px] border-[10px] border-[#0b0b0d] bg-black shadow-[0_60px_160px_-40px_rgba(255,255,255,0.22)] ring-1 ring-white/10 sm:rounded-[26px] sm:border-[16px]">
              {hasShot('tv_home') && (
                <img src="/shots/tv_home.webp" alt="The BoltOS TV home screen: a film of the Lofoten Islands, the apps in a slim glass pane on the left" width={1920} height={1080} className="block h-auto w-full" />
              )}
            </div>
            <div className="mx-auto h-3 w-1/3 rounded-b-2xl bg-gradient-to-b from-[#16161a] to-transparent sm:h-5" />
          </motion.div>
          <div className="mt-6 flex flex-col justify-between gap-2 text-[13px] text-fog sm:flex-row">
            <span>Home, as it comes up: the film, the apps, and the time in the corner.</span>
            <span className="font-mono">QEMU · 1920×1080 · screendump</span>
          </div>
        </Container>
      </div>
    </section>
  )
}

function Numbers() {
  return (
    <section className="pb-24 sm:pb-36">
      <Container>
        <div className="grid grid-cols-1 border-t border-l border-line sm:grid-cols-2 lg:grid-cols-4">
          {tvNumbers.map((n, i) => (
            <Reveal key={n.label} delay={i * 0.08} className="border-r border-b border-line p-6 sm:p-9">
              <div className="display text-[clamp(2.4rem,3.7vw,3.6rem)] whitespace-nowrap tabular-nums">
                <span className={i === 0 ? 'aurora-text' : undefined}>{n.value}</span>
                {n.unit && <span className="ml-1 text-[0.45em] text-fog">{n.unit}</span>}
              </div>
              <p className="mt-4 text-[14px] leading-snug text-smoke">{n.label}</p>
            </Reveal>
          ))}
        </div>
      </Container>
    </section>
  )
}

function Features() {
  return (
    <section className="py-12 sm:py-24">
      <Container>
        <SectionHead
          n="01"
          eyebrow="What is on it"
          title={<>Everything a TV does. <span className="serif-i text-smoke">Nothing it does not.</span></>}
          intro="Six screens, each one reachable with four arrows, OK and Back — which is what a remote has. A mouse or an air-remote works too."
        />
        <div className="mt-16 space-y-20 sm:mt-24 sm:space-y-32">
          {tvFeatures.map((f, i) => (
            <div key={f.title} className={cn('grid items-center gap-8 lg:grid-cols-[1fr_1.5fr] lg:gap-16', i % 2 === 1 && 'lg:grid-cols-[1.5fr_1fr]')}>
              <Reveal className={cn(i % 2 === 1 && 'lg:order-2')}>
                <p className="font-mono text-[11.5px] tracking-[0.14em] text-fog uppercase">{String(i + 1).padStart(2, '0')}</p>
                <h3 className="mt-4 text-[clamp(1.8rem,3vw,2.6rem)] font-medium tracking-[-0.035em]">{f.title}</h3>
                <p className="mt-5 max-w-lg text-[16px] leading-relaxed text-smoke">{f.body}</p>
              </Reveal>
              {f.shot && (
                <Reveal delay={0.1} className={cn(i % 2 === 1 && 'lg:order-1')}>
                  <Shot k={f.shot} colour caption={false} list={tvFeatures.map(x => x.shot!).filter(Boolean)} />
                </Reveal>
              )}
            </div>
          ))}
        </div>
      </Container>
    </section>
  )
}

const dlIcons = { 'tv-usb': Usb, 'tv-iso': Disc3 } as const

function Install() {
  return (
    <section id="install" className="scroll-mt-24 border-t border-line py-24 sm:py-36">
      <Container>
        <SectionHead
          n="02"
          eyebrow="Putting it on your TV"
          title={<>Four steps, <span className="serif-i text-smoke">one USB stick.</span></>}
          intro="BoltOS TV runs on a small computer plugged into the television, which becomes the set’s smart part. Both images download straight from boltos.dev."
        />
        <div className="mt-16 grid gap-4 lg:grid-cols-2">
          {tvDownloads.map((d, i) => {
            const Icon = dlIcons[d.id as keyof typeof dlIcons]
            const primary = i === 0
            return (
              <Reveal key={d.id} delay={i * 0.08}>
                <a href={d.href} className={cn('group flex h-full flex-col rounded-3xl border p-7 transition-all duration-500 sm:p-9', primary ? 'border-white bg-white text-black hover:bg-white/90' : 'border-line bg-ash hover:border-white/40')}>
                  <div className="flex items-center justify-between">
                    <Icon className="size-7" strokeWidth={1.4} />
                    <span className={cn('font-mono text-[11px]', primary ? 'text-black/50' : 'text-fog')}>{d.size}</span>
                  </div>
                  <h3 className="mt-12 text-[28px] font-medium tracking-[-0.035em]">{d.name}</h3>
                  <p className={cn('mt-1 font-mono text-[13px]', primary ? 'text-black/60' : 'text-fog')}>{d.file}</p>
                  <p className={cn('mt-5 flex-1 text-[15px] leading-relaxed', primary ? 'text-black/70' : 'text-smoke')}>{d.blurb}</p>
                  <p className={cn('mt-6 text-[12.5px]', primary ? 'text-black/50' : 'text-fog')}>Best for: {d.bestFor}</p>
                </a>
              </Reveal>
            )
          })}
        </div>
        <ol className="mt-16 grid gap-px overflow-hidden rounded-3xl border border-line bg-line sm:grid-cols-2">
          {tvSteps.map((s, i) => (
            <li key={s.t} className="bg-black p-7 sm:p-10">
              <p className="font-mono text-[11.5px] text-fog">STEP {i + 1}</p>
              <h3 className="mt-3 text-[22px] font-medium tracking-[-0.03em]">{s.t}</h3>
              <p className="mt-4 text-[15px] leading-relaxed text-smoke">{s.body}</p>
              {s.code && <CodeBlock className="mt-6" label="terminal" text={s.code} />}
            </li>
          ))}
        </ol>
      </Container>
    </section>
  )
}

function Remote() {
  return (
    <section className="py-12 sm:py-24">
      <Container>
        <SectionHead
          n="03"
          eyebrow="The remote"
          title={<>A keyboard, <span className="serif-i text-smoke">or a remote that is one.</span></>}
          intro="Any USB keyboard works, and so do the cheap 2.4 GHz air-mouse remotes that plug in with a dongle — they arrive as a keyboard and a mouse, over the kernel’s own xHCI driver."
        />
        <div className="mt-14 grid gap-px overflow-hidden rounded-2xl border border-line bg-line sm:grid-cols-2">
          {tvKeys.map(([k, v]) => (
            <div key={k} className="flex items-center justify-between gap-6 bg-black px-6 py-5">
              <span className="font-mono text-[13px] text-white">{k}</span>
              <span className="text-right text-[14px] text-smoke">{v}</span>
            </div>
          ))}
        </div>
      </Container>
    </section>
  )
}

function Limits() {
  return (
    <section className="py-12 sm:py-24">
      <Container>
        <SectionHead
          n="04"
          eyebrow="What it cannot do yet"
          title={<>Said plainly, <span className="serif-i text-smoke">up front.</span></>}
        />
        <div className="mt-14 grid gap-4 md:grid-cols-2">
          {tvLimits.map((l, i) => (
            <Reveal key={i} delay={i * 0.05} className="rounded-2xl border border-line bg-ash p-7 text-[15px] leading-relaxed text-smoke">
              {l}
            </Reveal>
          ))}
        </div>
      </Container>
    </section>
  )
}

function Credits() {
  return (
    <section className="py-12 sm:py-24">
      <Container>
        <SectionHead
          n="05"
          eyebrow="Credits"
          title={<>Made by <span className="serif-i text-smoke">Adrian Derakhshideh.</span></>}
          intro="The films are from Pexels, filmed in 4K by the people below and cut into seamless loops. MPEG-1 decoding is pl_mpeg by Dominic Szablewski (MIT); the type is Inter; the icons are Lucide (ISC)."
        />
        <div className="mt-14 grid grid-cols-1 gap-px overflow-hidden rounded-2xl border border-line bg-line sm:grid-cols-2 lg:grid-cols-3">
          {tvFilms.map(([f, who]) => (
            <div key={f} className="bg-black px-6 py-5">
              <p className="text-[16px] text-white">{f}</p>
              <p className="mt-1 text-[13px] text-fog">Filmed by {who}</p>
            </div>
          ))}
        </div>
      </Container>
    </section>
  )
}

function Cta() {
  const usb = tvDownloads[0]
  return (
    <section className="relative overflow-hidden border-t border-line py-28 sm:py-40">
      <div className="aurora-field pointer-events-none absolute inset-0 opacity-60" />
      <Container className="relative text-center">
        <TvIcon className="mx-auto size-10 text-white/70" strokeWidth={1.2} />
        <h2 className="display mt-6 text-[clamp(3.4rem,11vw,10rem)]">
          <MaskLines lines={[<>Switch it <span className="aurora-text">on.</span></>]} />
        </h2>
        <p className="mx-auto mt-8 max-w-lg text-[17px] leading-relaxed text-white/75">
          One USB stick, any small PC, any television with an HDMI input.
        </p>
        <div className="mt-10 flex flex-wrap justify-center gap-3">
          <Button to={usb.href} download><DownloadIcon className="size-4" /> {usb.file} <span className="text-black/50">{usb.size}</span></Button>
          <a href="#install" className="inline-flex h-12 items-center gap-2 rounded-full border border-line-strong px-6 text-[14px] font-medium text-white transition-colors hover:border-white hover:bg-white/5">How to install <ArrowDown className="size-4" /></a>
        </div>
      </Container>
    </section>
  )
}
