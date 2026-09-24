import { useState } from 'react'
import { Link } from 'react-router-dom'
import { ArrowUpRight, Check, Copy } from 'lucide-react'
import { MaskLines, Reveal } from './Reveal'
import { cn } from '@/lib/utils'

export function Container({ className, children }: { className?: string; children: React.ReactNode }) {
  return <div className={cn('mx-auto w-full max-w-[1320px] px-5 sm:px-8', className)}>{children}</div>
}

export function PageHero({ eyebrow, title, intro, children }: { eyebrow: string; title: React.ReactNode[]; intro?: React.ReactNode; children?: React.ReactNode }) {
  return (
    <section className="relative overflow-hidden pt-40 pb-20 sm:pt-48 sm:pb-24">
      <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-70" />
      <Container className="relative">
        <Reveal><p className="eyebrow">{eyebrow}</p></Reveal>
        <h1 className="display mt-6 text-[clamp(3rem,9vw,8.5rem)]">
          <MaskLines lines={title} />
        </h1>
        {intro && (
          <Reveal delay={0.25}>
            <p className="mt-10 max-w-2xl text-[clamp(1.05rem,1.6vw,1.3rem)] leading-relaxed text-smoke">{intro}</p>
          </Reveal>
        )}
        {children}
      </Container>
    </section>
  )
}

export function SectionHead({ n, eyebrow, title, intro, className }: { n?: string; eyebrow: string; title: React.ReactNode; intro?: React.ReactNode; className?: string }) {
  return (
    <div className={cn('grid gap-8 lg:grid-cols-[1fr_1.4fr] lg:gap-16', className)}>
      <Reveal>
        <p className="eyebrow flex items-center gap-3">
          {n && <span className="text-white">{n}</span>}
          <span className="h-px w-8 bg-line-strong" />
          {eyebrow}
        </p>
      </Reveal>
      <div>
        <Reveal><h2 className="display text-[clamp(2.2rem,5vw,4.5rem)]">{title}</h2></Reveal>
        {intro && <Reveal delay={0.1}><p className="mt-6 max-w-2xl text-[17px] leading-relaxed text-smoke">{intro}</p></Reveal>}
      </div>
    </div>
  )
}

export function CodeBlock({ label, text, className }: { label?: string; text: string; className?: string }) {
  const [copied, setCopied] = useState(false)
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(text)
      setCopied(true)
      setTimeout(() => setCopied(false), 1400)
    } catch { /* clipboard blocked; nothing to do */ }
  }
  return (
    <div className={cn('overflow-hidden rounded-xl border border-line bg-ash', className)}>
      <div className="flex items-center justify-between border-b border-line px-4 py-2.5">
        <div className="flex items-center gap-3">
          <span className="flex gap-1.5">
            <i className="size-2.5 rounded-full border border-white/25" />
            <i className="size-2.5 rounded-full border border-white/25" />
            <i className="size-2.5 rounded-full border border-white/25" />
          </span>
          {label && <span className="font-mono text-[11px] text-fog">{label}</span>}
        </div>
        <button onClick={copy} className="grid size-7 place-items-center rounded-md text-fog hover:bg-white/5 hover:text-white" aria-label="Copy">
          {copied ? <Check className="size-3.5" /> : <Copy className="size-3.5" />}
        </button>
      </div>
      <pre className="overflow-x-auto p-5 font-mono text-[12.5px] leading-[1.75] text-[#d4d4d4]"><code>{text}</code></pre>
    </div>
  )
}

export function ArrowLink({ to, children, className }: { to: string; children: React.ReactNode; className?: string }) {
  const cls = cn('group inline-flex items-center gap-2 text-[14px] text-white', className)
  const inner = (
    <>
      <span className="border-b border-white/30 pb-0.5 transition-colors group-hover:border-white">{children}</span>
      <ArrowUpRight className="size-4 transition-transform duration-500 group-hover:translate-x-0.5 group-hover:-translate-y-0.5" />
    </>
  )
  return to.startsWith('http') ? <a href={to} target="_blank" rel="noreferrer" className={cls}>{inner}</a> : <Link to={to} className={cls}>{inner}</Link>
}

export function Button({ to, children, variant = 'solid', className, download }: { to: string; children: React.ReactNode; variant?: 'solid' | 'ghost'; className?: string; download?: boolean }) {
  const cls = cn(
    'inline-flex h-12 items-center justify-center gap-2 rounded-full px-6 text-[14px] font-medium transition-all duration-500',
    variant === 'solid' ? 'bg-white text-black hover:bg-white/85' : 'border border-line-strong text-white hover:border-white hover:bg-white/5',
    className,
  )
  if (to.startsWith('http')) return <a href={to} className={cls} target={download ? undefined : '_blank'} rel="noreferrer">{children}</a>
  return <Link to={to} className={cls}>{children}</Link>
}
