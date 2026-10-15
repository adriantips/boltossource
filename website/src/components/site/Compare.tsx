import { useCallback, useRef, useState } from 'react'
import { ChevronsLeftRight } from 'lucide-react'
import { shotSrc, thumbSrc, shotSize, hasShot } from '@/data/shots'
import { cn } from '@/lib/utils'

type Props = {
  before: string
  after: string
  beforeLabel?: string
  afterLabel?: string
  start?: number
  thumb?: boolean
  className?: string
}

// Two screenshots of the same thing, one month apart, with a handle to drag
// between them. The old one sits on top and gets clipped, so the new one is
// what you see if the clip-path is ever unsupported.
export function Compare({ before, after, beforeLabel = 'Before', afterLabel = 'After', start = 50, thumb = false, className }: Props) {
  const [pos, setPos] = useState(start)
  const box = useRef<HTMLDivElement>(null)
  const dragging = useRef(false)

  const at = useCallback((clientX: number) => {
    const r = box.current?.getBoundingClientRect()
    if (!r) return
    setPos(Math.min(100, Math.max(0, ((clientX - r.left) / r.width) * 100)))
  }, [])

  if (!hasShot(before) || !hasShot(after)) return null
  const { w, h } = shotSize(after)
  const src = thumb ? thumbSrc : shotSrc

  return (
    <div
      ref={box}
      className={cn('group relative cursor-ew-resize select-none overflow-hidden rounded-xl border border-line-strong bg-coal touch-pan-y', className)}
      style={{ aspectRatio: `${w} / ${h}` }}
      onPointerDown={e => { dragging.current = true; e.currentTarget.setPointerCapture(e.pointerId); at(e.clientX) }}
      onPointerMove={e => { if (dragging.current) at(e.clientX) }}
      onPointerUp={() => { dragging.current = false }}
      onPointerCancel={() => { dragging.current = false }}
    >
      <img src={src(after)} alt={afterLabel} width={w} height={h} loading="lazy" draggable={false} className="absolute inset-0 block size-full object-cover" />
      <img
        src={src(before)}
        alt={beforeLabel}
        width={w}
        height={h}
        loading="lazy"
        draggable={false}
        className="absolute inset-0 block size-full object-cover"
        style={{ clipPath: `inset(0 ${100 - pos}% 0 0)` }}
      />

      <span className="pointer-events-none absolute top-3 left-3 rounded-full border border-white/15 bg-black/70 px-3 py-1 font-mono text-[10.5px] tracking-[0.14em] text-white/80 uppercase backdrop-blur" style={{ opacity: pos > 12 ? 1 : 0 }}>
        {beforeLabel}
      </span>
      <span className="aurora-chip pointer-events-none absolute top-3 right-3 rounded-full px-3 py-1 font-mono text-[10.5px] tracking-[0.14em] text-black uppercase" style={{ opacity: pos < 88 ? 1 : 0 }}>
        {afterLabel}
      </span>

      {/* the keyboard's way in: Tab to it, then the arrow keys */}
      <input
        type="range"
        min={0}
        max={100}
        value={Math.round(pos)}
        onChange={e => setPos(Number(e.target.value))}
        aria-label={`${beforeLabel} and ${afterLabel}: drag to compare`}
        className="peer pointer-events-none absolute inset-0 opacity-0"
      />
      <div className="pointer-events-none absolute inset-y-0 w-px bg-white shadow-[0_0_24px_rgba(255,255,255,0.8)] peer-focus-visible:[&>span]:ring-2 peer-focus-visible:[&>span]:ring-white" style={{ left: `${pos}%` }}>
        <span className="absolute top-1/2 left-1/2 grid size-11 -translate-x-1/2 -translate-y-1/2 place-items-center rounded-full border border-white/40 bg-black/60 text-white shadow-2xl backdrop-blur-md transition-transform group-active:scale-90">
          <ChevronsLeftRight className="size-4" />
        </span>
      </div>
    </div>
  )
}
