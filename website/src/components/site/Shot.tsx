import { Maximize2 } from 'lucide-react'
import { cn } from '@/lib/utils'
import { hasShot, shotSrc, thumbSrc, captionFor, shotSize } from '@/data/shots'
import { useLightbox } from './Lightbox'

type Props = {
  k: string
  list?: string[]
  className?: string
  imgClassName?: string
  caption?: string | false
  thumb?: boolean
  colour?: boolean
  eager?: boolean
}

// A screenshot in a thin frame. Monochrome until you hover it, click for full size.
export function Shot({ k, list, className, imgClassName, caption, thumb = false, colour = false, eager = false }: Props) {
  const { open } = useLightbox()
  if (!hasShot(k)) return null
  const { w, h } = shotSize(k)
  const text = caption === false ? null : caption ?? captionFor(k)
  return (
    <figure className={cn('group', className)}>
      <button
        type="button"
        onClick={() => open(k, list)}
        className="relative block w-full cursor-zoom-in overflow-hidden rounded-xl border border-line bg-coal text-left shadow-[0_40px_120px_-40px_rgba(255,255,255,0.12)]"
        aria-label={`Open screenshot: ${captionFor(k)}`}
      >
        <img
          src={thumb ? thumbSrc(k) : shotSrc(k)}
          width={w}
          height={h}
          alt={captionFor(k)}
          loading={eager ? 'eager' : 'lazy'}
          decoding="async"
          className={cn('block h-auto w-full', !colour && 'mono-img', 'group-hover:scale-[1.012]', imgClassName)}
        />
        <span className="pointer-events-none absolute right-3 top-3 grid size-8 place-items-center rounded-full border border-white/20 bg-black/60 opacity-0 backdrop-blur transition-opacity duration-500 group-hover:opacity-100">
          <Maximize2 className="size-3.5" />
        </span>
      </button>
      {text && <figcaption className="mt-3 text-[13px] leading-relaxed text-fog">{text}</figcaption>}
    </figure>
  )
}
