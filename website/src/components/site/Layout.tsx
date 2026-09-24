import { useEffect, useRef } from 'react'
import { Outlet, useLocation } from 'react-router-dom'
import Lenis from 'lenis'
import { Nav } from './Nav'
import { Footer } from './Footer'
import { LightboxProvider } from './Lightbox'
import Noise from '@/components/bits/Noise'

const titles: Record<string, string> = {
  '/': 'BoltOS — an operating system, written from nothing',
  '/system': 'Architecture — BoltOS',
  '/apps': 'Apps — BoltOS',
  '/gallery': 'Gallery — BoltOS',
  '/timeline': 'Timeline — BoltOS',
  '/creator': 'Adrian Derakhshideh — BoltOS',
  '/download': 'Download — BoltOS',
  '/docs': 'Docs — BoltOS',
}

export function Layout() {
  const { pathname } = useLocation()
  const lenis = useRef<Lenis | null>(null)

  useEffect(() => {
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return
    const l = new Lenis({ duration: 1.15, smoothWheel: true })
    lenis.current = l
    let raf = 0
    const loop = (t: number) => { l.raf(t); raf = requestAnimationFrame(loop) }
    raf = requestAnimationFrame(loop)
    return () => { cancelAnimationFrame(raf); l.destroy() }
  }, [])

  useEffect(() => {
    lenis.current?.scrollTo(0, { immediate: true })
    window.scrollTo(0, 0)
    if (titles[pathname]) document.title = titles[pathname]
  }, [pathname])

  return (
    <LightboxProvider>
      <div className="pointer-events-none fixed inset-0 z-[60] opacity-60 mix-blend-overlay">
        <Noise patternAlpha={14} patternRefreshInterval={3} />
      </div>
      <Nav />
      <main className="min-h-screen">
        <Outlet />
      </main>
      <Footer />
    </LightboxProvider>
  )
}
