import { useState } from 'react'
import { motion } from 'motion/react'
import { Container, PageHero } from '@/components/site/Blocks'
import { useLightbox } from '@/components/site/Lightbox'
import { hasShot, thumbSrc, captionFor, shotSize } from '@/data/shots'
import { cn } from '@/lib/utils'

const sets: { name: string; keys: string[] }[] = [
  { name: 'Desktop', keys: ['desktop', 'startmenu', 'quicksettings', 'notifications', 'tiled', 'startsearch', 'lock', 'login', 'terminal_sysinfo', 'terminal_net', 'terminal_ps', 'bootmenu'] },
  { name: 'Apps', keys: ['files_win', 'settings_win', 'taskmgr_win', 'clock_win', 'media_win', 'calc_win', 'mixer_win', 'stopwatch_win', 'software_win', 'sysinfo_win', 'browser_win', 'ide_win', 'photos_win', 'security_win', 'devmgr_win', 'network_win', 'logs_win', 'memory_win', 'installer_win', 'clipboard_win', 'music_win', 'paint_win', 'notepad_win', 'sheets_win', 'calendar_win', 'contacts_win', 'tasks_win', 'charmap_win', 'colorpick_win', 'snip_win', 'piano_win'] },
  { name: 'Games', keys: ['doom_play', 'strike_play', 'strike_max', 'doom_win', 'boltcraft_shot', 'gameboy_max', 'solitaire_max', 'mines', '2048', 'snake', 'ttt', 'life_max', 'matrix_max'] },
  { name: 'Full screen', keys: ['files_max', 'settings_max', 'browser_max', 'ide_max', 'taskmgr_max', 'media_max', 'clock_max', 'security_max', 'software_max', 'devmgr_max', 'logs_max', 'sheets_max', 'photos_max', 'calendar_max', 'solitaire_max', 'charmap_max', 'installer_max'] },
  { name: 'Before the revamp', keys: ['v1_desktop', 'v1_startmenu', 'v1_tiled', 'v1_files_max', 'v1_settings_max', 'v1_taskmgr_max', 'v1_clock_max', 'v1_media_max', 'v1_sysinfo_max', 'v1_browser_max', 'v1_ide_max', 'v1_login'] },
  { name: 'History', keys: ['legacy_desktop', 'legacy_icons', 'legacy_login', 'legacy_nssite', 'legacy_nsrender'] },
]

export default function Gallery() {
  const [tab, setTab] = useState(0)
  // colour by default now: the glass desktop is not much of a picture in grey
  const [colour, setColour] = useState(true)
  const { open } = useLightbox()
  const keys = sets[tab].keys.filter(hasShot)
  const total = new Set(sets.flatMap(s => s.keys.filter(hasShot))).size

  return (
    <>
      <PageHero
        eyebrow={`${total} screenshots`}
        title={['Gallery.']}
        intro="Every image here was captured from the current build — the glass desktop — booted in QEMU and driven over QMP at 1920×1080. No mock-ups, no retouching. Before the revamp is the desktop as it was in September; History is June 2026, before the project had a repository."
      />
      <Container className="pb-32">
        <div className="sticky top-20 z-30 mb-10 flex flex-wrap items-center justify-between gap-3">
          <div className="flex gap-1 overflow-x-auto rounded-full border border-line bg-black/70 p-1.5 backdrop-blur-xl">
            {sets.map((s, i) => (
              <button key={s.name} onClick={() => setTab(i)} className={cn('relative shrink-0 rounded-full px-4 py-2 text-[13px] transition-colors', tab === i ? 'text-black' : 'text-smoke hover:text-white')}>
                {tab === i && <motion.span layoutId="gal-pill" className="absolute inset-0 rounded-full bg-white" transition={{ type: 'spring', stiffness: 380, damping: 32 }} />}
                <span className="relative">{s.name}</span>
              </button>
            ))}
          </div>
          <button
            onClick={() => setColour(c => !c)}
            className="flex items-center gap-3 rounded-full border border-line bg-black/70 py-2 pr-4 pl-2 text-[13px] text-smoke backdrop-blur-xl hover:text-white"
            aria-pressed={colour}
          >
            <span className={cn('relative h-5 w-9 rounded-full border border-line-strong transition-colors', colour && 'bg-white')}>
              <span className={cn('absolute top-0.5 left-0.5 size-3.5 rounded-full bg-white transition-all', colour && 'translate-x-4 bg-black')} />
            </span>
            Colour
          </button>
        </div>

        <div className={cn('columns-1 gap-5 sm:columns-2 lg:columns-3', colour && 'colour')}>
          {keys.map((k, i) => {
            const { w, h } = shotSize(k)
            return (
              <motion.figure
                key={`${tab}-${k}`}
                initial={{ opacity: 0, y: 24 }}
                animate={{ opacity: 1, y: 0 }}
                transition={{ duration: 0.7, delay: Math.min(i * 0.04, 0.5), ease: [0.16, 1, 0.3, 1] }}
                className="group mb-5 break-inside-avoid"
              >
                <button onClick={() => open(k, keys)} className="block w-full cursor-zoom-in overflow-hidden rounded-xl border border-line bg-coal">
                  <img src={thumbSrc(k)} width={w} height={h} alt={captionFor(k)} loading="lazy" className="mono-img block h-auto w-full group-hover:scale-[1.02]" />
                </button>
                <figcaption className="mt-2.5 text-[12.5px] leading-snug text-fog">{captionFor(k)}</figcaption>
              </motion.figure>
            )
          })}
        </div>
      </Container>
    </>
  )
}
