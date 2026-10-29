import { useRef } from 'react'
import { motion, useScroll, useSpring, useTransform } from 'motion/react'
import { ArrowDown, Cpu, Download as DownloadIcon, HardDrive, Code2 } from 'lucide-react'
import { Container, Button, CodeBlock, SectionHead, ArrowLink } from '@/components/site/Blocks'
import { MaskLines, Reveal } from '@/components/site/Reveal'
import { Shot } from '@/components/site/Shot'
import { hasShot } from '@/data/shots'
import { mini, miniNumbers, miniFeatures, miniLimits, miniSteps, miniKeys, miniDownloads } from '@/data/mini'
import { cn } from '@/lib/utils'

export default function Mini() {
  return (
    <>
      <Hero />
      <Numbers />
      <Features />
      <Install />
      <Keys />
      <Limits />
      <Credits />
      <Cta />
    </>
  )
}

// The screen as the Mac shows it: Screen Sharing's window around the Pi's 1024x600.
function MacWindow({ children }: { children: React.ReactNode }) {
  return (
    <div className="overflow-hidden rounded-[12px] border border-white/12 bg-[#1b1c1f] shadow-[0_60px_160px_-40px_rgba(255,255,255,0.18)]">
      <div className="flex h-9 items-center gap-2 border-b border-white/10 px-4">
        <span className="size-3 rounded-full bg-[#ff5f57]" />
        <span className="size-3 rounded-full bg-[#febc2e]" />
        <span className="size-3 rounded-full bg-[#28c840]" />
        <span className="ml-3 text-[12.5px] font-medium text-white/70">BoltOS Mini</span>
      </div>
      {children}
    </div>
  )
}

function Hero() {
  const ref = useRef<HTMLDivElement>(null)
  const { scrollYProgress } = useScroll({ target: ref, offset: ['start end', 'center center'] })
  const p = useSpring(scrollYProgress, { stiffness: 90, damping: 24, mass: 0.4 })
  const rotateX = useTransform(p, [0, 1], [18, 0])
  const scale = useTransform(p, [0, 1], [0.9, 1])
  const card = miniDownloads[0]

  return (
    <section className="relative overflow-hidden pt-36 pb-24 sm:pt-44 sm:pb-36">
      <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-50" />
      <Container className="relative">
        <Reveal>
          <p className="inline-flex items-center gap-3 rounded-full border border-white/15 bg-black/40 py-1.5 pr-4 pl-1.5 backdrop-blur">
            <span className="rounded-full bg-white px-2.5 py-0.5 font-mono text-[10.5px] font-medium text-black">NEW</span>
            <span className="font-mono text-[11.5px] text-smoke">{mini.dates} · for the {mini.board}</span>
          </p>
        </Reveal>
        <h1 className="display mt-8 text-[clamp(4.2rem,16.5vw,17rem)] !leading-[0.86]">
          <MaskLines lines={['BoltOS', <span className="serif-i text-smoke">Mini.</span>]} delay={0.1} />
        </h1>
        <div className="mt-10 grid gap-10 lg:grid-cols-[1.3fr_1fr] lg:items-end">
          <Reveal delay={0.35}>
            <p className="max-w-2xl text-[clamp(1.1rem,1.7vw,1.4rem)] leading-relaxed text-white/80">
              BoltOS on a board the size of a matchbox. A 64-bit ARM kernel written from scratch for the Orange Pi Zero3 — four
              Cortex-A53 cores, one gigabyte — up a quarter of a second after U-Boot, with a code editor that runs Python, C and
              C++ on the board, files on the SD card, a browser, YouTube on the second core, Wi-Fi, Bluetooth, and apps from
              boltos.dev — among them a language model that runs on the board itself. Until it has a screen of its own, your
              Mac is its screen, over one USB-C cable.
            </p>
          </Reveal>
          <Reveal delay={0.5} className="flex flex-wrap gap-3 lg:justify-end">
            <Button to={card.href} download><DownloadIcon className="size-4" /> Get the card image</Button>
            <a href="#install" className="inline-flex h-12 items-center gap-2 rounded-full border border-line-strong px-6 text-[14px] font-medium text-white transition-colors hover:border-white hover:bg-white/5">
              How to install <ArrowDown className="size-4" />
            </a>
          </Reveal>
        </div>
      </Container>

      <div className="relative mt-20 sm:mt-28" style={{ perspective: 1400 }}>
        <Container>
          <motion.div ref={ref} style={{ rotateX, scale, transformOrigin: 'center top' }} className="relative mx-auto max-w-5xl">
            <MacWindow>
              {hasShot('mini_home') && (
                <img src="/shots/mini_home.webp" alt="BoltOS Mini's home screen: a bar of numbered apps along the top, the time, facts about the board, and the app tiles" width={1024} height={600} className="block h-auto w-full" />
              )}
            </MacWindow>
          </motion.div>
          <div className="mx-auto mt-6 flex max-w-5xl flex-col justify-between gap-2 text-[13px] text-fog sm:flex-row">
            <span>Home, in Screen Sharing on the Mac, straight off the Pi.</span>
            <span className="font-mono">Orange Pi Zero3 · 1024×600</span>
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
          {miniNumbers.map((n, i) => (
            <Reveal key={n.label} delay={i * 0.08} className="border-r border-b border-line p-6 sm:p-9">
              <div className="display text-[clamp(2.4rem,3.7vw,3.6rem)] whitespace-nowrap tabular-nums">
                {n.value}
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
          title={<>Eight apps on a small board, <span className="serif-i text-smoke">and more from boltos.dev.</span></>}
          intro="Everything runs in one loop on one core — the USB ports, the network, the screen. The other three wait for work: a program from Code, a playing video’s decoding, or a language model’s arithmetic. Nothing waits on anything else."
        />
        <div className="mt-16 space-y-20 sm:mt-24 sm:space-y-32">
          {miniFeatures.map((f, i) => (
            <div key={f.title} className={cn('grid items-center gap-8 lg:grid-cols-[1fr_1.5fr] lg:gap-16', i % 2 === 1 && 'lg:grid-cols-[1.5fr_1fr]')}>
              <Reveal className={cn(i % 2 === 1 && 'lg:order-2')}>
                <p className="font-mono text-[11.5px] tracking-[0.14em] text-fog uppercase">{String(i + 1).padStart(2, '0')}</p>
                <h3 className="mt-4 text-[clamp(1.8rem,3vw,2.6rem)] font-medium tracking-[-0.035em]">{f.title}</h3>
                <p className="mt-5 max-w-lg text-[16px] leading-relaxed text-smoke">{f.body}</p>
              </Reveal>
              {f.shot && (
                <Reveal delay={0.1} className={cn(i % 2 === 1 && 'lg:order-1')}>
                  <Shot k={f.shot} colour caption={false} list={miniFeatures.map(x => x.shot!).filter(Boolean)} />
                </Reveal>
              )}
            </div>
          ))}
        </div>
      </Container>
    </section>
  )
}

const dlIcons = { 'mini-card': HardDrive, 'mini-src': Code2 } as const

function Install() {
  return (
    <section id="install" className="scroll-mt-24 border-t border-line py-24 sm:py-36">
      <Container>
        <SectionHead
          n="02"
          eyebrow="Putting it on the Pi"
          title={<>Four steps, <span className="serif-i text-smoke">one card, one cable.</span></>}
          intro="After the first card, it stays in the Pi: new versions go over the same USB-C cable while U-Boot waits two seconds at every start."
        />
        <div className="mt-16 grid gap-4 lg:grid-cols-2">
          {miniDownloads.map((d, i) => {
            const Icon = dlIcons[d.id as keyof typeof dlIcons]
            const primary = i === 0
            const outside = d.href.startsWith('http')
            return (
              <Reveal key={d.id} delay={i * 0.08}>
                <a href={d.href} target={outside ? '_blank' : undefined} rel={outside ? 'noreferrer' : undefined}
                  className={cn('group flex h-full flex-col rounded-3xl border p-7 transition-all duration-500 sm:p-9', primary ? 'border-white bg-white text-black hover:bg-white/90' : 'border-line bg-ash hover:border-white/40')}>
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
          {miniSteps.map((s, i) => (
            <li key={s.t} className="bg-black p-7 sm:p-10">
              <p className="font-mono text-[11.5px] text-fog">STEP {i + 1}</p>
              <h3 className="mt-3 text-[22px] font-medium tracking-[-0.03em]">{s.t}</h3>
              <p className="mt-4 text-[15px] leading-relaxed text-smoke">{s.body}</p>
              {s.code && <CodeBlock className="mt-6" label="terminal" text={s.code} />}
            </li>
          ))}
        </ol>
        <ArrowLink to={mini.guide} className="mt-10">The whole guide, MINI.md</ArrowLink>
      </Container>
    </section>
  )
}

function Keys() {
  return (
    <section className="py-12 sm:py-24">
      <Container>
        <SectionHead
          n="03"
          eyebrow="Keys"
          title={<>The Mac’s keyboard, <span className="serif-i text-smoke">or the one on the Pi.</span></>}
          intro="A USB keyboard on the Pi’s USB-A port, through a hub if you like, and the Mac’s keyboard and trackpad through Screen Sharing, all at once. Click an app in the bar to open it."
        />
        <div className="mt-14 grid gap-px overflow-hidden rounded-2xl border border-line bg-line sm:grid-cols-2">
          {miniKeys.map(([k, v]) => (
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
          {miniLimits.map((l, i) => (
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
          intro="The board boots through U-Boot and Trusted Firmware-A. The Wi-Fi and Bluetooth firmware is Unisoc’s, fetched from Orange Pi when Mini is built. The type is Inter and DejaVu Sans Mono; the video in the screenshot is Big Buck Bunny, © Blender Foundation, CC BY 3.0."
        />
      </Container>
    </section>
  )
}

function Cta() {
  const card = miniDownloads[0]
  return (
    <section className="relative overflow-hidden border-t border-line py-28 sm:py-40">
      <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-40" />
      <Container className="relative text-center">
        <Cpu className="mx-auto size-10 text-white/70" strokeWidth={1.2} />
        <h2 className="display mt-6 text-[clamp(3.4rem,11vw,10rem)]">
          <MaskLines lines={[<>Plug it <span className="serif-i text-smoke">in.</span></>]} />
        </h2>
        <p className="mx-auto mt-8 max-w-lg text-[17px] leading-relaxed text-white/75">
          One microSD card, one Orange Pi Zero3, one USB-C cable to a Mac.
        </p>
        <div className="mt-10 flex flex-wrap justify-center gap-3">
          <Button to={card.href} download><DownloadIcon className="size-4" /> {card.file} <span className="text-black/50">{card.size}</span></Button>
          <a href="#install" className="inline-flex h-12 items-center gap-2 rounded-full border border-line-strong px-6 text-[14px] font-medium text-white transition-colors hover:border-white hover:bg-white/5">How to install <ArrowDown className="size-4" /></a>
        </div>
      </Container>
    </section>
  )
}
