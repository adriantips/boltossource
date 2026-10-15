import { useRef } from 'react'
import { motion, useScroll, useSpring } from 'motion/react'
import { GitCommitHorizontal } from 'lucide-react'
import { Container, PageHero, ArrowLink } from '@/components/site/Blocks'
import { Reveal } from '@/components/site/Reveal'
import { Shot } from '@/components/site/Shot'
import { eras } from '@/data/timeline'
import { links } from '@/data/site'
import { hasShot } from '@/data/shots'

export default function Timeline() {
  const ref = useRef<HTMLDivElement>(null)
  const { scrollYProgress } = useScroll({ target: ref, offset: ['start 60%', 'end 60%'] })
  const fill = useSpring(scrollYProgress, { stiffness: 120, damping: 30 })
  const shots = eras.flatMap(e => e.items.map(i => i.shot)).filter((k): k is string => !!k && hasShot(k))

  return (
    <>
      <PageHero
        eyebrow="June – October 2026"
        title={['From a boot', <>sector to <span className="serif-i text-smoke">this.</span></>]}
        intro="How BoltOS got here, from the oldest file in the tree to the current build. Commit messages are quoted exactly as they were written."
      />

      <Container className="pb-32">
        <div ref={ref} className="relative">
          {/* the rail, and how far down it you have read */}
          <div className="absolute top-0 bottom-0 left-[7px] w-px bg-line sm:left-[calc(12rem+7px)]" />
          <motion.div style={{ scaleY: fill }} className="absolute top-0 bottom-0 left-[7px] w-px origin-top bg-white sm:left-[calc(12rem+7px)]" />

          {eras.map(era => (
            <section key={era.name} className="relative pb-20">
              <Reveal className="relative mb-12 pl-10 sm:pl-[calc(12rem+3rem)]">
                <span className="absolute top-2 left-0 grid size-[15px] place-items-center rounded-full border border-white bg-black sm:left-48">
                  <span className="size-[5px] rounded-full bg-white" />
                </span>
                <p className="eyebrow">{era.range}</p>
                <h2 className="display mt-3 text-[clamp(2.2rem,5vw,4.2rem)]">{era.name}</h2>
                <p className="mt-4 max-w-xl text-[16px] leading-relaxed text-smoke">{era.blurb}</p>
              </Reveal>

              <div className="space-y-16">
                {era.items.map(m => (
                  <Reveal key={m.date + m.title} className="relative grid gap-3 pl-10 sm:grid-cols-[12rem_1fr] sm:gap-0 sm:pl-0">
                    <span className="absolute top-[9px] left-[4px] size-[7px] rounded-full bg-white/50 sm:left-[calc(12rem+4px)]" />
                    <div className="sm:pr-10 sm:text-right">
                      <p className="font-mono text-[13px] text-white">{m.date}</p>
                      {m.tag && <span className="mt-2 inline-block rounded-full border border-line px-2.5 py-0.5 font-mono text-[10px] tracking-wider text-fog uppercase">{m.tag}</span>}
                    </div>
                    <div className="sm:pl-12">
                      <h3 className="text-[clamp(1.4rem,2.4vw,1.9rem)] font-medium tracking-[-0.035em]">{m.title}</h3>
                      <p className="mt-3 max-w-2xl text-[16px] leading-relaxed text-smoke">{m.body}</p>
                      {m.commits && (
                        <ul className="mt-6 max-w-2xl divide-y divide-line overflow-hidden rounded-xl border border-line bg-ash">
                          {m.commits.map(c => (
                            <li key={c} className="flex items-start gap-3 px-4 py-3 font-mono text-[12.5px] leading-relaxed text-[#d4d4d4]">
                              <GitCommitHorizontal className="mt-0.5 size-4 shrink-0 text-fog" />
                              <span>{c}</span>
                            </li>
                          ))}
                        </ul>
                      )}
                      {m.shot && hasShot(m.shot) && <Shot k={m.shot} list={shots} className="mt-8 max-w-3xl" thumb />}
                    </div>
                  </Reveal>
                ))}
              </div>
            </section>
          ))}
        </div>
        <div className="pl-10 sm:pl-[calc(12rem+3rem)]">
          <ArrowLink to={links.commits}>Every commit, on GitHub</ArrowLink>
        </div>
      </Container>
    </>
  )
}
