import { useRef, useState } from 'react'
import { motion, useScroll, useSpring, useTransform } from 'motion/react'
import { ArrowDown, ArrowUpRight, Download as DownloadIcon, GitCommitHorizontal } from 'lucide-react'
import { Container, Button, ArrowLink } from '@/components/site/Blocks'
import { MaskLines, Reveal } from '@/components/site/Reveal'
import { Compare } from '@/components/site/Compare'
import { Shot } from '@/components/site/Shot'
import { useLightbox } from '@/components/site/Lightbox'
import { revamp, pairs, numbers, features, rebuilt, found, commits } from '@/data/revamp'
import { downloads, links } from '@/data/site'
import { hasShot, thumbSrc, shotSize, captionFor } from '@/data/shots'
import { cn } from '@/lib/utils'

export default function Revamp() {
  return (
    <>
      <Hero />
      <Numbers />
      <BeforeAfter />
      <Features />
      <Rebuilt />
      <EveryApp />
      <Found />
      <TwoDays />
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

  return (
    <section className="relative overflow-hidden pt-36 pb-24 sm:pt-44 sm:pb-36">
      <div className="aurora-field pointer-events-none absolute -inset-x-20 -top-20 h-[900px] opacity-90" />
      <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-40" />
      <Container className="relative">
        <Reveal>
          <p className="inline-flex items-center gap-3 rounded-full border border-white/15 bg-black/40 py-1.5 pr-4 pl-1.5 backdrop-blur">
            <span className="aurora-chip rounded-full px-2.5 py-0.5 font-mono text-[10.5px] font-medium text-black">NEW</span>
            <span className="font-mono text-[11.5px] text-smoke">{revamp.dates} · {commits.length} commits</span>
          </p>
        </Reveal>
        <h1 className="display mt-8 text-[clamp(4.2rem,16.5vw,17rem)] !leading-[0.86]">
          <MaskLines lines={['The huge', <span className="aurora-text">UI revamp.</span>]} delay={0.1} />
        </h1>
        <div className="mt-10 grid gap-10 lg:grid-cols-[1.3fr_1fr] lg:items-end">
          <Reveal delay={0.35}>
            <p className="max-w-2xl text-[clamp(1.1rem,1.7vw,1.4rem)] leading-relaxed text-white/80">
              BoltOS has a new face. Liquid glass, real type, real icons, a dock, Quick Settings, a notification centre and nine
              rebuilt apps, all drawn by a compositor with no GPU to lean on, in a kernel written from nothing.
            </p>
          </Reveal>
          <Reveal delay={0.5} className="flex flex-wrap gap-3 lg:justify-end">
            <Button to="/download"><DownloadIcon className="size-4" /> Get the new build</Button>
            <a href="#before-after" className="inline-flex h-12 items-center gap-2 rounded-full border border-line-strong px-6 text-[14px] font-medium text-white transition-colors hover:border-white hover:bg-white/5">
              Before and after <ArrowDown className="size-4" />
            </a>
          </Reveal>
        </div>
      </Container>

      <div className="relative mt-20 sm:mt-28" style={{ perspective: 1400 }}>
        <Container>
          <motion.div ref={ref} style={{ rotateX, scale, transformOrigin: 'center top' }} className="relative">
            <div className="aurora-field absolute -inset-10 opacity-70" />
            <div className="glass relative overflow-hidden rounded-2xl p-1.5 sm:rounded-[22px] sm:p-2">
              <img src="/shots/desktop.webp" alt="The glass desktop at 1920 by 1080" width={1920} height={1080} className="block h-auto w-full rounded-xl sm:rounded-[16px]" />
            </div>
          </motion.div>
          <div className="mt-6 flex flex-col justify-between gap-2 text-[13px] text-fog sm:flex-row">
            <span>The desktop as it boots today: dock in the middle, desktop switcher on the left, tray on the right.</span>
            <span className="font-mono">QEMU · 1920×1080 · no GPU</span>
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
          {numbers.map((n, i) => (
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

function BeforeAfter() {
  const items = pairs.filter(p => hasShot(p.before) && hasShot(p.after))
  const [i, setI] = useState(0)
  const cur = items[i]
  if (!cur) return null
  return (
    <section id="before-after" className="scroll-mt-24 py-12 sm:py-24">
      <Container>
        <div className="grid gap-8 lg:grid-cols-[1fr_1.4fr] lg:gap-16">
          <Reveal>
            <p className="eyebrow flex items-center gap-3"><span className="text-white">01</span><span className="h-px w-8 bg-line-strong" />Before and after</p>
          </Reveal>
          <div>
            <Reveal><h2 className="display text-[clamp(2.4rem,5.5vw,5rem)]">Drag it. <span className="serif-i text-smoke">One month apart.</span></h2></Reveal>
            <Reveal delay={0.1}>
              <p className="mt-6 max-w-2xl text-[17px] leading-relaxed text-smoke">
                Left of the line is the desktop that shipped in September. Right of it is the one that boots now. Same machine,
                same resolution, same apps.
              </p>
            </Reveal>
          </div>
        </div>

        <div className="mt-14 flex gap-1 overflow-x-auto rounded-full border border-line bg-black/70 p-1.5 sm:inline-flex">
          {items.map((p, j) => (
            <button
              key={p.after}
              onClick={() => setI(j)}
              className={cn('relative shrink-0 rounded-full px-4 py-2 text-[13px] transition-colors', i === j ? 'text-black' : 'text-smoke hover:text-white')}
            >
              {i === j && <motion.span layoutId="ba-pill" className="aurora-chip absolute inset-0 rounded-full" transition={{ type: 'spring', stiffness: 380, damping: 32 }} />}
              <span className="relative">{p.title}</span>
            </button>
          ))}
        </div>

        <Reveal className="mt-6">
          <Compare key={cur.after} before={cur.before} after={cur.after} beforeLabel="September" afterLabel="October" start={52} />
        </Reveal>
        <div className="mt-6 grid gap-4 sm:grid-cols-[14rem_1fr]">
          <p className="text-[20px] font-medium tracking-[-0.03em]">{cur.title}</p>
          <p className="max-w-3xl text-[15.5px] leading-relaxed text-smoke">{cur.note}</p>
        </div>
      </Container>
    </section>
  )
}

function Features() {
  const items = features.filter(f => !f.shot || hasShot(f.shot))
  const list = items.map(f => f.shot).filter((k): k is string => !!k)
  return (
    <section className="py-24 sm:py-36">
      <Container>
        <div className="grid gap-8 lg:grid-cols-[1fr_1.4fr] lg:gap-16">
          <Reveal>
            <p className="eyebrow flex items-center gap-3"><span className="text-white">02</span><span className="h-px w-8 bg-line-strong" />What changed</p>
          </Reveal>
          <Reveal><h2 className="display text-[clamp(2.4rem,5.5vw,5rem)]">Not a new coat of paint. <span className="serif-i text-smoke">A new compositor.</span></h2></Reveal>
        </div>
        <div className="mt-20 space-y-28 sm:space-y-40">
          {items.map((f, i) => (
            <div key={f.title} className="grid items-center gap-10 lg:grid-cols-12 lg:gap-16">
              <Reveal className={i % 2 ? 'lg:order-2 lg:col-span-5' : 'lg:col-span-5'}>
                <p className="font-mono text-[11px] text-fog">{String(i + 1).padStart(2, '0')} · {f.mono}</p>
                <h3 className="mt-5 text-[clamp(2rem,3.6vw,3.2rem)] font-medium leading-[1.02] tracking-[-0.045em]">{f.title}</h3>
                <p className="mt-6 text-[16.5px] leading-relaxed text-smoke">{f.body}</p>
              </Reveal>
              <Reveal delay={0.1} className={i % 2 ? 'relative lg:order-1 lg:col-span-7' : 'relative lg:col-span-7'}>
                <div className="aurora-field absolute -inset-8 opacity-50" />
                {f.shot && <Shot k={f.shot} list={list} caption={false} colour className="relative" />}
              </Reveal>
            </div>
          ))}
        </div>
      </Container>
    </section>
  )
}

function Rebuilt() {
  const items = rebuilt.filter(r => hasShot(r.k))
  const { open } = useLightbox()
  return (
    <section className="relative overflow-hidden border-y border-line py-24 sm:py-36">
      <div className="aurora-field pointer-events-none absolute inset-0 opacity-40" />
      <Container className="relative">
        <div className="grid gap-8 lg:grid-cols-[1fr_1.4fr] lg:gap-16">
          <Reveal>
            <p className="eyebrow flex items-center gap-3"><span className="text-white">03</span><span className="h-px w-8 bg-line-strong" />Rebuilt</p>
          </Reveal>
          <div>
            <Reveal><h2 className="display text-[clamp(2.4rem,5.5vw,5rem)]">Nine apps, <span className="serif-i">started again.</span></h2></Reveal>
            <Reveal delay={0.1}>
              <p className="mt-6 max-w-2xl text-[17px] leading-relaxed text-smoke">
                Every app got Inter, the new palette and the new icons for free. These nine were taken apart and put back
                together instead: Settings, File Explorer and Calculator on the new control kit, and Clock, Media, Mixer,
                Task Manager, Stopwatch and Software rebuilt by hand.
              </p>
            </Reveal>
          </div>
        </div>
        <div className="mt-16 grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
          {items.map((r, i) => {
            const { w, h } = shotSize(r.k)
            return (
              <Reveal key={r.k} delay={(i % 3) * 0.07}>
                <button onClick={() => open(r.k, items.map(x => x.k))} className="glass group block w-full cursor-zoom-in overflow-hidden rounded-2xl p-1.5 text-left">
                  <img src={thumbSrc(r.k)} width={w} height={h} alt={captionFor(r.k)} loading="lazy" className="block h-auto w-full rounded-xl transition-transform duration-700 group-hover:scale-[1.02]" />
                  <span className="flex items-center justify-between px-3 py-3 text-[14px]">
                    {r.name} <ArrowUpRight className="size-4 text-fog transition-colors group-hover:text-white" />
                  </span>
                </button>
              </Reveal>
            )
          })}
        </div>
      </Container>
    </section>
  )
}

// every windowed capture from the new build, in a slow drifting wall
const wall = ['browser_win', 'ide_win', 'photos_win', 'paint_win', 'notepad_win', 'sheets_win', 'calendar_win', 'contacts_win', 'tasks_win', 'music_win', 'piano_win', 'charmap_win', 'colorpick_win', 'security_win', 'devmgr_win', 'logs_win', 'memory_win', 'network_win', 'installer_win', 'clipboard_win', 'snip_win', 'sysinfo_win', 'solitaire_win', 'mines_win', '2048_win', 'gameboy_win', 'life_win', 'snake_win']

function EveryApp() {
  const keys = wall.filter(hasShot)
  const { open } = useLightbox()
  if (keys.length < 6) return null
  const rows = [keys.filter((_, i) => i % 2 === 0), keys.filter((_, i) => i % 2 === 1)]
  return (
    <section className="overflow-hidden py-24 sm:py-36">
      <Container>
        <div className="grid gap-8 lg:grid-cols-[1fr_1.4fr] lg:gap-16">
          <Reveal>
            <p className="eyebrow flex items-center gap-3"><span className="text-white">04</span><span className="h-px w-8 bg-line-strong" />Everything else</p>
          </Reveal>
          <div>
            <Reveal><h2 className="display text-[clamp(2.4rem,5.5vw,5rem)]">And the other thirty-odd.</h2></Reveal>
            <Reveal delay={0.1}><p className="mt-6 max-w-2xl text-[17px] leading-relaxed text-smoke">Each one captured from the new build. Click any of them.</p></Reveal>
          </div>
        </div>
      </Container>
      <div className="mt-16 space-y-4">
        {rows.map((row, r) => (
          <div key={r} className="flex w-max gap-4" style={{ animation: `marquee ${row.length * 6}s linear infinite${r ? ' reverse' : ''}` }}>
            {[...row, ...row].map((k, i) => {
              const { w, h } = shotSize(k)
              return (
                <button key={k + i} onClick={() => open(k, keys)} className="glass block w-[300px] shrink-0 cursor-zoom-in overflow-hidden rounded-xl p-1 sm:w-[420px]" tabIndex={i < row.length ? 0 : -1}>
                  <img src={thumbSrc(k)} width={w} height={h} alt={captionFor(k)} loading="lazy" className="block h-auto w-full rounded-lg" />
                </button>
              )
            })}
          </div>
        ))}
      </div>
      <Container>
        <div className="mt-14 flex justify-center">
          <Button to="/gallery" variant="ghost">The whole gallery <ArrowUpRight className="size-4" /></Button>
        </div>
      </Container>
    </section>
  )
}

function Found() {
  return (
    <section className="py-24 sm:py-36">
      <Container>
        <div className="grid gap-8 lg:grid-cols-[1fr_1.4fr] lg:gap-16">
          <Reveal>
            <p className="eyebrow flex items-center gap-3"><span className="text-white">05</span><span className="h-px w-8 bg-line-strong" />Found on the way</p>
          </Reveal>
          <div>
            <Reveal><h2 className="display text-[clamp(2.4rem,5.5vw,5rem)]">A redesign is a <span className="serif-i">bug hunt.</span></h2></Reveal>
            <Reveal delay={0.1}>
              <p className="mt-6 max-w-2xl text-[17px] leading-relaxed text-smoke">
                Changing what every pixel is drawn with shakes loose everything that was quietly depending on the old way.
                Six of the things that fell out.
              </p>
            </Reveal>
          </div>
        </div>
        <Reveal className="mt-16 grid gap-px overflow-hidden rounded-2xl border border-line bg-line sm:grid-cols-2 lg:grid-cols-3">
          {found.map(f => (
            <div key={f.title} className="flex flex-col bg-black p-8">
              <a href={`${links.repo}/commit/${f.commit}`} target="_blank" rel="noreferrer" className="w-fit font-mono text-[11px] text-fog hover:text-white">{f.commit}</a>
              <h3 className="mt-10 text-[21px] font-medium leading-snug tracking-[-0.03em]">{f.title}</h3>
              <p className="mt-3 text-[14.5px] leading-relaxed text-smoke">
                {f.body.split('`').map((t, j) => (j % 2 ? <code key={j} className="inline-code">{t}</code> : t))}
              </p>
            </div>
          ))}
        </Reveal>
      </Container>
    </section>
  )
}

function TwoDays() {
  return (
    <section className="py-12 sm:py-24">
      <Container>
        <div className="grid gap-12 lg:grid-cols-[1fr_1.4fr] lg:gap-16">
          <div>
            <Reveal><p className="eyebrow flex items-center gap-3"><span className="text-white">06</span><span className="h-px w-8 bg-line-strong" />The log</p></Reveal>
            <Reveal><h2 className="display mt-6 text-[clamp(2.4rem,5vw,4.4rem)]">Thirteen commits, <span className="serif-i text-smoke">one evening and a day.</span></h2></Reveal>
            <Reveal delay={0.1}>
              <p className="mt-6 max-w-md text-[16px] leading-relaxed text-smoke">
                Quoted exactly as they were written. Nineteen GUI tests, plus the BIOS, UEFI and USB-stick boots, pass on the
                last one.
              </p>
            </Reveal>
            <ArrowLink to={links.commits} className="mt-8">Read them on GitHub</ArrowLink>
          </div>
          <Reveal>
            <ol className="divide-y divide-line overflow-hidden rounded-2xl border border-line bg-ash">
              {commits.map(c => (
                <li key={c.h}>
                  <a href={`${links.repo}/commit/${c.h}`} target="_blank" rel="noreferrer" className="group grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 px-5 py-4 transition-colors hover:bg-white/[0.03] sm:grid-cols-[auto_7.5rem_1fr]">
                    <GitCommitHorizontal className="mt-0.5 size-4 text-fog group-hover:text-white" />
                    <span className="font-mono text-[12px] text-fog sm:pt-px">{c.t}</span>
                    <span className="col-start-2 font-mono text-[12.5px] leading-relaxed text-[#d4d4d4] group-hover:text-white sm:col-start-3">{c.s}</span>
                  </a>
                </li>
              ))}
            </ol>
          </Reveal>
        </div>
      </Container>
    </section>
  )
}

function Cta() {
  const vdi = downloads.find(d => d.id === 'vdi')!
  return (
    <section className="relative overflow-hidden border-t border-line py-28 sm:py-40">
      <div className="aurora-field pointer-events-none absolute inset-0 opacity-60" />
      <Container className="relative text-center">
        <h2 className="display text-[clamp(3.4rem,11vw,10rem)]">
          <MaskLines lines={[<>See it <span className="aurora-text">yourself.</span></>]} />
        </h2>
        <p className="mx-auto mt-8 max-w-lg text-[17px] leading-relaxed text-white/75">
          The images on GitHub are this build. Boot one in QEMU or VirtualBox, or write it to a USB stick.
        </p>
        <div className="mt-10 flex flex-wrap justify-center gap-3">
          <Button to={vdi.href} download><DownloadIcon className="size-4" /> {vdi.file} <span className="text-black/50">{vdi.size}</span></Button>
          <Button to="/download" variant="ghost">All downloads</Button>
        </div>
      </Container>
    </section>
  )
}
