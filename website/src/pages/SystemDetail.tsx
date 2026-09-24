import { useEffect, useState } from 'react'
import { Link, Navigate, useParams } from 'react-router-dom'
import { ArrowLeft, ArrowRight, ArrowUpRight, FileCode2 } from 'lucide-react'
import { Container, CodeBlock } from '@/components/site/Blocks'
import { MaskLines, Reveal } from '@/components/site/Reveal'
import { Shot } from '@/components/site/Shot'
import { Rich } from '@/components/site/Rich'
import { systems, systemBySlug } from '@/data/systems'
import { links } from '@/data/site'
import { hasShot, pick } from '@/data/shots'
import { cn } from '@/lib/utils'

const anchor = (t: string) => t.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/(^-|-$)/g, '')

export default function SystemDetail() {
  const { slug = '' } = useParams()
  const s = systemBySlug[slug]
  const [active, setActive] = useState('')

  useEffect(() => {
    if (s) document.title = `${s.title} — BoltOS`
  }, [s])

  useEffect(() => {
    if (!s) return
    const els = s.sections.map(sec => document.getElementById(anchor(sec.title))).filter(Boolean) as HTMLElement[]
    const io = new IntersectionObserver(
      entries => entries.forEach(e => e.isIntersecting && setActive(e.target.id)),
      { rootMargin: '-35% 0px -60% 0px' },
    )
    els.forEach(el => io.observe(el))
    return () => io.disconnect()
  }, [s])

  if (!s) return <Navigate to="/system" replace />
  const i = systems.indexOf(s)
  const prev = systems[(i - 1 + systems.length) % systems.length]
  const next = systems[(i + 1) % systems.length]
  const hero = pick(s.shot, '_max')
  const secShot = (k: string) => pick(k, '_win', '_max')
  const pageShots = [hero, ...s.sections.map(x => x.shot && secShot(x.shot))].filter((k, j, a) => k && hasShot(k) && a.indexOf(k) === j) as string[]

  return (
    <article key={s.slug}>
      <header className="relative overflow-hidden pt-36 pb-16 sm:pt-44">
        <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-60" />
        <div className="pointer-events-none absolute -top-10 right-0 select-none font-semibold leading-none tracking-[-0.08em] text-white/[0.035] text-[42vw] sm:text-[30vw]" aria-hidden>
          {s.n}
        </div>
        <Container className="relative">
          <Reveal>
            <Link to="/system" className="eyebrow inline-flex items-center gap-2 hover:text-white">
              <ArrowLeft className="size-3.5" /> Architecture <span className="text-white/20">/</span> <span className="text-white">{s.n}</span>
            </Link>
          </Reveal>
          <h1 className="display mt-8 text-[clamp(3rem,8.5vw,8rem)]">
            <MaskLines lines={[s.title]} />
          </h1>
          <Reveal delay={0.2}>
            <p className="mt-6 serif-i text-[clamp(1.5rem,2.6vw,2.2rem)] leading-tight text-smoke">{s.kicker}</p>
          </Reveal>
          <Reveal delay={0.3}>
            <p className="mt-8 max-w-3xl text-[18px] leading-relaxed text-smoke">{s.intro}</p>
          </Reveal>
          <Reveal delay={0.35}>
            <dl className="mt-14 grid grid-cols-2 border-t border-l border-line lg:grid-cols-4">
              {s.facts.map(f => (
                <div key={f.k} className="border-r border-b border-line p-5 sm:p-6">
                  <dt className="eyebrow">{f.k}</dt>
                  <dd className="mt-3 text-[17px] font-medium tracking-[-0.02em]">{f.v}</dd>
                </div>
              ))}
            </dl>
          </Reveal>
        </Container>
      </header>

      {hasShot(hero) && (
        <Container className="pb-8">
          <Reveal>
            <Shot k={hero} list={pageShots} eager />
          </Reveal>
        </Container>
      )}

      <Container className="grid gap-16 py-20 lg:grid-cols-[15rem_1fr] lg:gap-20">
        <aside className="hidden lg:block">
          <div className="sticky top-28">
            <p className="eyebrow mb-5">On this page</p>
            <ul className="space-y-1 border-l border-line">
              {s.sections.map(sec => {
                const id = anchor(sec.title)
                return (
                  <li key={id}>
                    <a
                      href={`#${id}`}
                      onClick={e => { e.preventDefault(); document.getElementById(id)?.scrollIntoView({ behavior: 'smooth', block: 'start' }) }}
                      className={cn('-ml-px block border-l py-1.5 pl-4 text-[13px] leading-snug transition-colors', active === id ? 'border-white text-white' : 'border-transparent text-fog hover:text-smoke')}
                    >
                      {sec.title}
                    </a>
                  </li>
                )
              })}
            </ul>
            <p className="eyebrow mt-10 mb-4">Source</p>
            <ul className="space-y-2">
              {s.source.map(p => (
                <li key={p}>
                  <a href={p.includes('*') ? links.tree(p.split('/')[0]) : p.endsWith('/') ? links.tree(p.slice(0, -1)) : links.blob(p)} target="_blank" rel="noreferrer" className="inline-flex items-center gap-2 font-mono text-[12px] text-fog hover:text-white">
                    <FileCode2 className="size-3.5 shrink-0" /> {p}
                  </a>
                </li>
              ))}
            </ul>
          </div>
        </aside>

        <div className="min-w-0 max-w-3xl space-y-24">
          {s.sections.map((sec, j) => (
            <section key={sec.title} id={anchor(sec.title)} className="scroll-mt-28">
              <Reveal>
                <p className="font-mono text-[11px] text-fog">{s.n}.{j + 1}</p>
                <h2 className="mt-3 text-[clamp(1.8rem,3.2vw,2.6rem)] font-medium leading-[1.08] tracking-[-0.04em]">{sec.title}</h2>
              </Reveal>
              <Reveal delay={0.08} className="prose-bolt mt-7">
                {sec.body.map((p, k) => <p key={k}><Rich text={p} /></p>)}
              </Reveal>
              {sec.list && (
                <ul className="mt-6 space-y-2">
                  {sec.list.map(li => <li key={li} className="text-smoke"><Rich text={li} /></li>)}
                </ul>
              )}
              {sec.code && (
                <Reveal delay={0.1} className="mt-8">
                  <CodeBlock label={sec.code.label} text={sec.code.text} />
                </Reveal>
              )}
              {sec.shot && sec.shot !== s.shot && hasShot(secShot(sec.shot)) && (
                <Reveal delay={0.1} className="mt-10">
                  <Shot k={secShot(sec.shot)} list={pageShots} caption={sec.caption} />
                </Reveal>
              )}
            </section>
          ))}

          <div className="lg:hidden">
            <p className="eyebrow mb-4">Source</p>
            <div className="flex flex-wrap gap-2">
              {s.source.map(p => (
                <a key={p} href={links.tree(p.split('/')[0])} target="_blank" rel="noreferrer" className="rounded-full border border-line px-3 py-1.5 font-mono text-[11.5px] text-fog">{p}</a>
              ))}
            </div>
          </div>
        </div>
      </Container>

      <nav className="border-t border-line">
        <Container className="grid sm:grid-cols-2">
          <Link to={`/system/${prev.slug}`} className="group border-b border-line py-10 sm:border-r sm:border-b-0 sm:pr-10">
            <span className="eyebrow inline-flex items-center gap-2"><ArrowLeft className="size-3.5" /> Previous · {prev.n}</span>
            <p className="mt-4 text-[clamp(1.5rem,3vw,2.4rem)] font-medium tracking-[-0.035em] transition-transform duration-700 group-hover:-translate-x-2">{prev.title}</p>
          </Link>
          <Link to={`/system/${next.slug}`} className="group py-10 text-right sm:pl-10">
            <span className="eyebrow inline-flex items-center gap-2">Next · {next.n} <ArrowRight className="size-3.5" /></span>
            <p className="mt-4 text-[clamp(1.5rem,3vw,2.4rem)] font-medium tracking-[-0.035em] transition-transform duration-700 group-hover:translate-x-2">{next.title}</p>
          </Link>
        </Container>
      </nav>
      <Container className="flex justify-center pb-20 pt-4">
        <a href={links.repo} target="_blank" rel="noreferrer" className="inline-flex items-center gap-1.5 text-[13px] text-fog hover:text-white">
          Read the full README on GitHub <ArrowUpRight className="size-3.5" />
        </a>
      </Container>
    </article>
  )
}
