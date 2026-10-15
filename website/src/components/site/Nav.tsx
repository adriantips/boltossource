import { useEffect, useState } from 'react'
import { Link, NavLink, useLocation } from 'react-router-dom'
import { AnimatePresence, motion } from 'motion/react'
import { ArrowUpRight, Menu, X } from 'lucide-react'
import { Logo } from './Logo'
import { GitHubIcon } from './GitHubIcon'
import { links } from '@/data/site'
import { systems } from '@/data/systems'
import { cn } from '@/lib/utils'

const primary: { to: string; label: string; isNew?: boolean }[] = [
  { to: '/revamp', label: 'Revamp', isNew: true },
  { to: '/system', label: 'System' },
  { to: '/apps', label: 'Apps' },
  { to: '/gallery', label: 'Gallery' },
  { to: '/timeline', label: 'Timeline' },
  { to: '/creator', label: 'Creator' },
  { to: '/docs', label: 'Docs' },
]

export function Nav() {
  const [scrolled, setScrolled] = useState(false)
  const [open, setOpen] = useState(false)
  const [mega, setMega] = useState(false)
  const { pathname } = useLocation()

  useEffect(() => {
    const on = () => setScrolled(window.scrollY > 24)
    on()
    window.addEventListener('scroll', on, { passive: true })
    return () => window.removeEventListener('scroll', on)
  }, [])
  useEffect(() => { setOpen(false); setMega(false) }, [pathname])

  return (
    <header className="fixed inset-x-0 top-0 z-50">
      <div
        className={cn(
          'mx-auto mt-3 flex h-14 max-w-[1320px] items-center justify-between rounded-full px-3 pl-5 transition-all duration-700 sm:mt-4',
          'mx-3 sm:mx-6 xl:mx-auto',
          scrolled || open || mega ? 'border border-line bg-black/70 backdrop-blur-xl' : 'border border-transparent',
        )}
        onMouseLeave={() => setMega(false)}
      >
        <Link to="/" aria-label="BoltOS home" className="flex shrink-0 items-center">
          <Logo />
        </Link>

        <nav className="hidden items-center gap-1 lg:flex">
          {primary.map(item => (
            <NavLink
              key={item.to}
              to={item.to}
              onMouseEnter={() => setMega(item.to === '/system')}
              className={({ isActive }) =>
                cn(
                  'rounded-full px-3.5 py-2 text-[13.5px] transition-colors',
                  isActive ? 'text-white' : 'text-smoke hover:text-white',
                )
              }
            >
              {item.label}
              {item.isNew && <span className="aurora-chip ml-1.5 inline-block size-1.5 -translate-y-2 rounded-full" aria-label="new" />}
            </NavLink>
          ))}
        </nav>

        <div className="flex items-center gap-2">
          <a
            href={links.repo}
            target="_blank"
            rel="noreferrer"
            className="hidden items-center gap-2 rounded-full border border-line px-3.5 py-2 text-[13px] text-smoke transition-colors hover:border-white/40 hover:text-white sm:inline-flex"
          >
            <GitHubIcon className="size-4" /> GitHub
          </a>
          <Link
            to="/download"
            className="inline-flex items-center gap-1.5 rounded-full bg-white px-4 py-2 text-[13px] font-medium text-black transition-transform hover:scale-[1.03]"
          >
            Download <ArrowUpRight className="size-3.5" />
          </Link>
          <button
            className="grid size-10 place-items-center rounded-full border border-line lg:hidden"
            onClick={() => setOpen(o => !o)}
            aria-label={open ? 'Close menu' : 'Open menu'}
            aria-expanded={open}
          >
            {open ? <X className="size-4" /> : <Menu className="size-4" />}
          </button>
        </div>
      </div>

      {/* System mega menu */}
      <AnimatePresence>
        {mega && (
          <motion.div
            initial={{ opacity: 0, y: -8 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -8 }}
            transition={{ duration: 0.3, ease: [0.16, 1, 0.3, 1] }}
            onMouseEnter={() => setMega(true)}
            onMouseLeave={() => setMega(false)}
            className="mx-auto mt-2 hidden max-w-[1320px] rounded-3xl border border-line bg-black/85 p-3 backdrop-blur-2xl lg:block xl:mx-auto mx-6"
          >
            <div className="grid grid-cols-4 gap-1">
              {systems.map(s => (
                <Link
                  key={s.slug}
                  to={`/system/${s.slug}`}
                  className="group rounded-2xl p-4 transition-colors hover:bg-white/[0.05]"
                >
                  <div className="flex items-baseline gap-3">
                    <span className="font-mono text-[11px] text-fog">{s.n}</span>
                    <span className="text-[14px] font-medium">{s.title}</span>
                  </div>
                  <p className="mt-1.5 pl-8 text-[12.5px] leading-snug text-fog group-hover:text-smoke">{s.kicker}</p>
                </Link>
              ))}
              <Link to="/system" className="flex items-center justify-center gap-2 rounded-2xl border border-dashed border-line p-4 text-[13px] text-smoke hover:text-white">
                The whole architecture <ArrowUpRight className="size-3.5" />
              </Link>
            </div>
          </motion.div>
        )}
      </AnimatePresence>

      {/* Mobile menu */}
      <AnimatePresence>
        {open && (
          <motion.div
            initial={{ opacity: 0, y: -10 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -10 }}
            className="mx-3 mt-2 max-h-[calc(100dvh-90px)] overflow-y-auto rounded-3xl border border-line bg-black/95 p-3 backdrop-blur-2xl sm:mx-6 lg:hidden"
          >
            {[{ to: '/', label: 'Home' }, ...primary, { to: '/download', label: 'Download' }].map(item => (
              <Link key={item.to} to={item.to} className="flex items-center justify-between rounded-2xl px-4 py-3.5 text-[22px] tracking-tight hover:bg-white/[0.05]">
                <span className="flex items-center gap-3">
                  {item.label}
                  {'isNew' in item && item.isNew && <span className="aurora-chip rounded-full px-2 py-0.5 font-mono text-[10px] text-black">NEW</span>}
                </span>
                <ArrowUpRight className="size-4 text-fog" />
              </Link>
            ))}
            <div className="mt-2 border-t border-line px-4 pt-4 pb-2">
              <p className="eyebrow mb-3">Subsystems</p>
              <div className="grid grid-cols-2 gap-x-4 gap-y-2.5">
                {systems.map(s => (
                  <Link key={s.slug} to={`/system/${s.slug}`} className="text-[14px] text-smoke">
                    <span className="mr-2 font-mono text-[11px] text-fog">{s.n}</span>{s.short}
                  </Link>
                ))}
              </div>
            </div>
            <a href={links.repo} target="_blank" rel="noreferrer" className="mt-3 flex items-center justify-center gap-2 rounded-2xl border border-line py-3.5 text-[14px]">
              <GitHubIcon className="size-4" /> View on GitHub
            </a>
          </motion.div>
        )}
      </AnimatePresence>
    </header>
  )
}
