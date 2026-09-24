import { Link } from 'react-router-dom'
import { ArrowUpRight } from 'lucide-react'
import { Mark } from './Logo'
import { links } from '@/data/site'
import { systems } from '@/data/systems'

export function Footer() {
  return (
    <footer className="relative overflow-hidden border-t border-line bg-black">
      <div className="mx-auto max-w-[1320px] px-5 pt-20 pb-10 sm:px-8">
        <div className="grid gap-14 lg:grid-cols-[1.3fr_2fr]">
          <div>
            <Mark className="size-12" />
            <p className="mt-6 max-w-sm text-[15px] leading-relaxed text-smoke">
              A 64-bit operating system written from scratch in C and x86-64 assembly by Adrian Derakhshideh.
            </p>
            <div className="mt-8 flex flex-wrap gap-2">
              <a href={links.repo} target="_blank" rel="noreferrer" className="inline-flex items-center gap-1.5 rounded-full border border-line px-4 py-2 text-[13px] text-smoke hover:text-white hover:border-white/40 transition-colors">
                Source on GitHub <ArrowUpRight className="size-3.5" />
              </a>
              <Link to="/download" className="inline-flex items-center gap-1.5 rounded-full bg-white px-4 py-2 text-[13px] font-medium text-black">
                Download
              </Link>
            </div>
          </div>
          <div className="grid grid-cols-2 gap-10 sm:grid-cols-4">
            <FooterCol title="Explore" items={[
              ['/system', 'Architecture'], ['/apps', 'Apps'], ['/gallery', 'Gallery'], ['/timeline', 'Timeline'], ['/creator', 'Creator'],
            ]} />
            <FooterCol title="System" items={systems.slice(0, 7).map(s => [`/system/${s.slug}`, s.short])} />
            <FooterCol title="More" items={systems.slice(7).map(s => [`/system/${s.slug}`, s.short])} />
            <FooterCol title="Get it" items={[
              ['/download', 'Download'], ['/docs', 'Docs'], [links.guide, 'User guide'], [links.developing, 'Developing'], [links.roadmap, 'Roadmap'],
            ]} />
          </div>
        </div>
      </div>
      <div className="pointer-events-none select-none px-3 text-center font-semibold leading-[0.78] tracking-[-0.07em] text-white/[0.055] text-[25vw]" aria-hidden>
        BoltOS
      </div>
      <div className="mx-auto flex max-w-[1320px] flex-col gap-2 border-t border-line px-5 py-6 text-[12px] text-fog sm:flex-row sm:justify-between sm:px-8">
        <span>© {new Date().getFullYear()} Adrian Derakhshideh. Screenshots are from the current build.</span>
        <span className="font-mono">No GRUB · No Linux · No borrowed kernel</span>
      </div>
    </footer>
  )
}

function FooterCol({ title, items }: { title: string; items: string[][] }) {
  return (
    <div>
      <p className="eyebrow mb-5">{title}</p>
      <ul className="space-y-3">
        {items.map(([href, label]) => (
          <li key={href}>
            {href.startsWith('http') ? (
              <a href={href} target="_blank" rel="noreferrer" className="text-[14px] text-smoke hover:text-white transition-colors">{label}</a>
            ) : (
              <Link to={href} className="text-[14px] text-smoke hover:text-white transition-colors">{label}</Link>
            )}
          </li>
        ))}
      </ul>
    </div>
  )
}
