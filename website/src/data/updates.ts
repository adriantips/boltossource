import { useEffect, useState } from 'react'

// Each edition's newest version, as release.sh publishes it on this very site
// (/update/<channel>.json, kept in R2 and served by worker.js). BoltOS asks for
// the same files when it starts, so what this page says is what a machine sees.
export type Release = {
  version: string
  date: string
  kind: 'quick' | 'big'
  size: number
  notes: string
}
export type Latest = Release & { name: string; url: string; sha256: string }

export const editions = [
  {
    id: 'mini',
    name: 'BoltOS Mini',
    where: 'Settings → Updates',
    blurb: 'On the Orange Pi Zero3. The new kernel goes onto the SD card beside the old one and the Pi restarts into it.',
  },
  {
    id: 'tv',
    name: 'BoltOS TV',
    where: 'Settings → Updates',
    blurb: 'On the USB stick the TV started from, for both legacy BIOS and UEFI. The stick image has room for two kernels.',
  },
  {
    id: 'desktop',
    name: 'BoltOS',
    where: 'Settings → Updates',
    blurb: 'The desktop, on the stick or the disk it was installed to: the EFI partition and the boot header switch over together.',
  },
] as const

export type EditionId = (typeof editions)[number]['id']

const cache = new Map<string, Promise<unknown>>()
function load<T>(path: string): Promise<T | null> {
  if (!cache.has(path))
    cache.set(path, fetch(path, { cache: 'no-cache' }).then(r => (r.ok ? r.json() : null)).catch(() => null))
  return cache.get(path) as Promise<T | null>
}

export function useLatest(id: EditionId) {
  const [v, setV] = useState<Latest | null | undefined>(undefined)
  useEffect(() => { load<Latest>(`/update/${id}.json`).then(setV) }, [id])
  return v                                     // undefined while loading, null if none
}

export function useHistory(id: EditionId) {
  const [v, setV] = useState<Release[] | null | undefined>(undefined)
  useEffect(() => { load<Release[]>(`/update/${id}-history.json`).then(setV) }, [id])
  return v
}

export const mb = (b: number) => `${(b / 1e6).toFixed(1)} MB`
export const lines = (notes: string) => notes.split('\n').map(s => s.trim()).filter(Boolean)
