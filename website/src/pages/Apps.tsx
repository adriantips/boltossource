import { useMemo, useState } from 'react'
import { AnimatePresence, motion } from 'motion/react'
import { ArrowUpRight } from 'lucide-react'
import { Container, PageHero } from '@/components/site/Blocks'
import { Reveal } from '@/components/site/Reveal'
import { useLightbox } from '@/components/site/Lightbox'
import { apps, type AppCategory } from '@/data/apps'
import { hasShot, pick, thumbSrc } from '@/data/shots'
import { links } from '@/data/site'
import { cn } from '@/lib/utils'

const cats: ('All' | AppCategory)[] = ['All', 'System', 'Internet', 'Create', 'Media', 'Productivity', 'Games']

// The window crop reads best at card size; the full desktop is the fallback.
const best = (k?: string) => (k ? pick(k, '_win') : k)

export default function Apps() {
  const [cat, setCat] = useState<(typeof cats)[number]>('All')
  const { open } = useLightbox()
  const shown = useMemo(() => apps.filter(a => cat === 'All' || a.cat === cat), [cat])
  const list = shown.map(a => best(a.shot)).filter((k): k is string => !!k && hasShot(k))

  return (
    <>
      <PageHero
        eyebrow="The apps"
        title={['Forty-three apps.', <span className="serif-i text-smoke">Not one ported.</span>]}
        intro="Every app on the desktop is a C file in the kernel tree, drawn by BoltOS's own graphics routines — except the ones that prove the SDK works, like boltcraft, which the kernel does not link at all."
      />

      <Container className="pb-32">
        <div className="sticky top-20 z-30 -mx-2 mb-12 flex gap-1 overflow-x-auto rounded-full border border-line bg-black/70 p-1.5 backdrop-blur-xl sm:mx-0 sm:w-fit">
          {cats.map(c => (
            <button
              key={c}
              onClick={() => setCat(c)}
              className={cn('relative shrink-0 rounded-full px-4 py-2 text-[13px] transition-colors', cat === c ? 'text-black' : 'text-smoke hover:text-white')}
            >
              {cat === c && <motion.span layoutId="cat-pill" className="absolute inset-0 rounded-full bg-white" transition={{ type: 'spring', stiffness: 380, damping: 32 }} />}
              <span className="relative">{c}</span>
            </button>
          ))}
        </div>

        <motion.div layout className="grid gap-x-6 gap-y-14 sm:grid-cols-2 lg:grid-cols-3">
          <AnimatePresence mode="popLayout">
            {shown.map(a => {
              const k = best(a.shot)
              const ok = !!k && hasShot(k)
              return (
                <motion.article
                  layout
                  key={a.name}
                  initial={{ opacity: 0, y: 20 }}
                  animate={{ opacity: 1, y: 0 }}
                  exit={{ opacity: 0, scale: 0.96 }}
                  transition={{ duration: 0.5, ease: [0.16, 1, 0.3, 1] }}
                  className="group"
                >
                  <button
                    type="button"
                    disabled={!ok}
                    onClick={() => ok && open(k!, list)}
                    className="relative block aspect-[16/10] w-full overflow-hidden rounded-2xl border border-line bg-coal text-left enabled:cursor-zoom-in"
                    aria-label={`Screenshot of ${a.name}`}
                  >
                    {ok ? (
                      <img src={thumbSrc(k!)} alt={`${a.name} on BoltOS`} loading="lazy" className="mono-img size-full object-cover object-top group-hover:scale-[1.03]" />
                    ) : (
                      <div className="grid size-full place-items-center">
                        <span className="display text-[64px] text-white/10">{a.name.slice(0, 2)}</span>
                      </div>
                    )}
                    <span className="absolute top-3 left-3 rounded-full border border-white/15 bg-black/60 px-2.5 py-1 font-mono text-[10px] tracking-wider text-smoke uppercase backdrop-blur">{a.cat}</span>
                  </button>
                  <div className="mt-5 flex items-start justify-between gap-4">
                    <div>
                      <h3 className="text-[20px] font-medium tracking-[-0.03em]">{a.name}</h3>
                      <p className="mt-1.5 text-[14px] leading-relaxed text-smoke">{a.blurb}</p>
                    </div>
                    <a href={a.src.endsWith('/') ? links.tree(a.src.slice(0, -1)) : links.blob(a.src)} target="_blank" rel="noreferrer" className="mt-1 grid size-8 shrink-0 place-items-center rounded-full border border-line text-fog transition-colors hover:border-white hover:text-white" aria-label={`${a.name} source`} title={a.src}>
                      <ArrowUpRight className="size-3.5" />
                    </a>
                  </div>
                </motion.article>
              )
            })}
          </AnimatePresence>
        </motion.div>

        <Reveal className="mt-28 rounded-3xl border border-line p-8 sm:p-12">
          <p className="eyebrow">Anatomy of an app</p>
          <div className="mt-6 grid gap-8 lg:grid-cols-2">
            <p className="text-[17px] leading-relaxed text-smoke">
              An app is one <code className="inline-code">kernel/app_*.c</code> file that registers a window and five callbacks —
              <code className="inline-code">draw</code>, <code className="inline-code">key</code>, <code className="inline-code">click</code>,
              <code className="inline-code">drag</code> and <code className="inline-code">tick</code>. It draws with the compositor's vector helpers into a backbuffer;
              the compositor handles focus, z-order, snapping, the taskbar and virtual desktops. Programs that should not live in the kernel
              are built with the SDK instead and run in ring 3.
            </p>
            <pre className="overflow-x-auto rounded-xl border border-line bg-ash p-5 font-mono text-[12.5px] leading-[1.75] text-[#d4d4d4]">{`window_t *win = gui_add_window("Piano", 460, 300,
                               0x9B6CF2, ICON_PIANO);
win->draw  = piano_draw;
win->key   = piano_key;
win->click = piano_click;`}</pre>
          </div>
        </Reveal>
      </Container>
    </>
  )
}
