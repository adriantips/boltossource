import { lazy, Suspense } from 'react'
import { BrowserRouter, Route, Routes } from 'react-router-dom'
import { Layout } from '@/components/site/Layout'
import Home from '@/pages/Home'

const Revamp = lazy(() => import('@/pages/Revamp'))
const SystemOverview = lazy(() => import('@/pages/SystemOverview'))
const SystemDetail = lazy(() => import('@/pages/SystemDetail'))
const Apps = lazy(() => import('@/pages/Apps'))
const Gallery = lazy(() => import('@/pages/Gallery'))
const Timeline = lazy(() => import('@/pages/Timeline'))
const Creator = lazy(() => import('@/pages/Creator'))
const Download = lazy(() => import('@/pages/Download'))
const Docs = lazy(() => import('@/pages/Docs'))
const NotFound = lazy(() => import('@/pages/NotFound'))

export default function App() {
  return (
    <BrowserRouter>
      <Suspense fallback={<div className="min-h-screen bg-black" />}>
        <Routes>
          <Route element={<Layout />}>
            <Route index element={<Home />} />
            <Route path="revamp" element={<Revamp />} />
            <Route path="system" element={<SystemOverview />} />
            <Route path="system/:slug" element={<SystemDetail />} />
            <Route path="apps" element={<Apps />} />
            <Route path="gallery" element={<Gallery />} />
            <Route path="timeline" element={<Timeline />} />
            <Route path="creator" element={<Creator />} />
            <Route path="download" element={<Download />} />
            <Route path="docs" element={<Docs />} />
            <Route path="*" element={<NotFound />} />
          </Route>
        </Routes>
      </Suspense>
    </BrowserRouter>
  )
}
