import { createContext, useCallback, useContext, useEffect, useState } from 'react'
import { AnimatePresence, motion } from 'motion/react'
import { ArrowLeft, ArrowRight, X } from 'lucide-react'
import { shotSrc, captionFor } from '@/data/shots'

type Ctx = { open: (key: string, list?: string[]) => void }
const LightboxCtx = createContext<Ctx>({ open: () => {} })
export const useLightbox = () => useContext(LightboxCtx)

export function LightboxProvider({ children }: { children: React.ReactNode }) {
  const [list, setList] = useState<string[]>([])
  const [idx, setIdx] = useState(-1)
  const key = idx >= 0 ? list[idx] : null

  const open = useCallback((k: string, l?: string[]) => {
    const items = l && l.includes(k) ? l : [k]
    setList(items)
    setIdx(items.indexOf(k))
  }, [])
  const close = () => setIdx(-1)
  const step = useCallback((d: number) => setIdx(i => (i < 0 ? i : (i + d + list.length) % list.length)), [list.length])

  useEffect(() => {
    if (idx < 0) return
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setIdx(-1)
      if (e.key === 'ArrowRight') step(1)
      if (e.key === 'ArrowLeft') step(-1)
    }
    window.addEventListener('keydown', onKey)
    document.documentElement.style.overflow = 'hidden'
    return () => {
      window.removeEventListener('keydown', onKey)
      document.documentElement.style.overflow = ''
    }
  }, [idx, step])

  return (
    <LightboxCtx.Provider value={{ open }}>
      {children}
      <AnimatePresence>
        {key && (
          <motion.div
            className="fixed inset-0 z-[100] flex flex-col bg-black/95 backdrop-blur-xl"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            transition={{ duration: 0.35 }}
            onClick={close}
          >
            <div className="flex items-center justify-between px-5 py-4 sm:px-8">
              <span className="eyebrow">
                {String(idx + 1).padStart(2, '0')} / {String(list.length).padStart(2, '0')}
              </span>
              <button aria-label="Close" className="grid size-10 place-items-center rounded-full border border-line text-white hover:bg-white hover:text-black transition-colors" onClick={close}>
                <X className="size-4" />
              </button>
            </div>
            <div className="relative flex min-h-0 flex-1 items-center justify-center px-4 sm:px-20">
              <motion.img
                key={key}
                src={shotSrc(key)}
                alt={captionFor(key)}
                className="max-h-full max-w-full rounded-md border border-line object-contain shadow-2xl"
                initial={{ opacity: 0, scale: 0.97 }}
                animate={{ opacity: 1, scale: 1 }}
                transition={{ duration: 0.5, ease: [0.16, 1, 0.3, 1] }}
                onClick={e => e.stopPropagation()}
              />
              {list.length > 1 && (
                <>
                  <button aria-label="Previous" onClick={e => { e.stopPropagation(); step(-1) }} className="absolute left-3 hidden size-11 place-items-center rounded-full border border-line bg-black/60 hover:bg-white hover:text-black transition-colors sm:grid sm:left-6">
                    <ArrowLeft className="size-4" />
                  </button>
                  <button aria-label="Next" onClick={e => { e.stopPropagation(); step(1) }} className="absolute right-3 hidden size-11 place-items-center rounded-full border border-line bg-black/60 hover:bg-white hover:text-black transition-colors sm:grid sm:right-6">
                    <ArrowRight className="size-4" />
                  </button>
                </>
              )}
            </div>
            <p className="px-5 py-5 text-center text-sm text-smoke sm:px-8">{captionFor(key)}</p>
          </motion.div>
        )}
      </AnimatePresence>
    </LightboxCtx.Provider>
  )
}
