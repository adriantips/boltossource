// Put the BoltOS TV images on boltos.dev itself, so they download from the
// site rather than from GitHub. Workers static assets refuse any file over
// 25 MiB and the TV images are ~37 MiB (the films do not compress), so each
// one is cut into parts here and worker.js stitches them back into a single
// response. Runs after `vite build`; an image that has not been built is
// skipped, and its download link then answers 404 rather than half a file.
import fs from 'node:fs'
import path from 'node:path'

const root = path.resolve(import.meta.dirname, '..')
const iso = path.resolve(root, '..', 'iso')
const out = path.join(root, 'dist', 'dl')
const PART = 20 * 1024 * 1024
const files = ['boltostv-usb.img.gz', 'boltos-tv.iso']

fs.mkdirSync(out, { recursive: true })
const manifest = {}
for (const name of files) {
  const src = path.join(iso, name)
  if (!fs.existsSync(src)) { console.log(`downloads: ${name} not built, skipped`); continue }
  const buf = fs.readFileSync(src)
  const parts = []
  for (let off = 0, i = 0; off < buf.length; off += PART, i++) {
    const part = `${name}.part${i}`
    fs.writeFileSync(path.join(out, part), buf.subarray(off, Math.min(off + PART, buf.length)))
    parts.push(part)
  }
  manifest[name] = { size: buf.length, parts }
  console.log(`downloads: ${name} ${(buf.length / 1048576).toFixed(1)} MiB in ${parts.length} parts`)
}
fs.writeFileSync(path.join(out, 'manifest.json'), JSON.stringify(manifest))
