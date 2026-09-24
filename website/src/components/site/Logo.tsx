import { cn } from '@/lib/utils'

// The bolt through a broken ring, redrawn from logo.png so it stays sharp at
// any size. The two arcs stop where the bolt crosses them.
export function Mark({ className, strokeWidth = 3.2 }: { className?: string; strokeWidth?: number }) {
  return (
    <svg viewBox="12 12 76 76" className={cn('size-6', className)} fill="none" aria-hidden>
      <path d="M61.9 28 A23.8 23.8 0 0 1 46.3 72.1" stroke="currentColor" strokeWidth={strokeWidth} strokeLinecap="butt" />
      <path d="M38.6 69.5 A23.8 23.8 0 0 1 53.3 25" stroke="currentColor" strokeWidth={strokeWidth} strokeLinecap="butt" />
      <path d="M63.2 18.2 L36.5 52 L48.2 52 L37.8 78 L63.6 44.8 L51.4 44.8 Z" fill="currentColor" />
    </svg>
  )
}

export function Logo({ className }: { className?: string }) {
  return (
    <span className={cn('inline-flex items-center gap-2.5', className)}>
      <Mark className="size-7" />
      <span className="text-[17px] font-semibold tracking-[-0.03em]">BoltOS</span>
    </span>
  )
}
