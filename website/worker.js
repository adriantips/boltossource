// boltos.dev is a static site; this script exists for two things.
//
// /dl/<name>: Workers static assets cap a file at 25 MiB, so the BoltOS TV
// and Mini images are stored in parts (scripts/downloads.mjs) and served
// here as one stream, with the length up front so a browser shows real
// progress.
//
// /pkg/<name>: BoltOS Mini's packages, from the R2 bucket bound as PKG --
// index.json (what the Packages app lists) and the .bpk files it installs.
// The Pi asks with a Range so a download can carry on where it stopped.
//
// Everything else never reaches this script: wrangler.jsonc runs it first
// only for /dl/* and /pkg/*.
async function pkg(request, env, url) {
  const key = decodeURIComponent(url.pathname.slice(5))
  if (!key || key.includes('..')) return new Response('Not found', { status: 404 })
  const type = key.endsWith('.json') ? 'application/json; charset=utf-8' : 'application/octet-stream'
  const cache = key.endsWith('.json') ? 'public, max-age=60' : 'public, max-age=86400'
  const head = await env.PKG.head(key)
  if (!head) return new Response('Not found', { status: 404 })
  const size = head.size
  const headers = { 'content-type': type, 'cache-control': cache, 'accept-ranges': 'bytes', etag: head.httpEtag }
  const m = /^bytes=(\d+)-(\d*)$/.exec(request.headers.get('range') || '')
  if (m) {
    const start = Number(m[1]), end = m[2] ? Math.min(Number(m[2]), size - 1) : size - 1
    if (start >= size || end < start)
      return new Response(null, { status: 416, headers: { 'content-range': `bytes */${size}` } })
    headers['content-range'] = `bytes ${start}-${end}/${size}`
    headers['content-length'] = String(end - start + 1)
    if (request.method === 'HEAD') return new Response(null, { status: 206, headers })
    const obj = await env.PKG.get(key, { range: { offset: start, length: end - start + 1 } })
    return new Response(obj.body, { status: 206, headers })
  }
  headers['content-length'] = String(size)
  if (request.method === 'HEAD') return new Response(null, { headers })
  const obj = await env.PKG.get(key)
  return new Response(obj.body, { headers })
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url)
    if (url.pathname.startsWith('/pkg/')) return pkg(request, env, url)
    if (!url.pathname.startsWith('/dl/')) return env.ASSETS.fetch(request)

    const name = decodeURIComponent(url.pathname.slice(4))
    const m = await env.ASSETS.fetch(new URL('/dl/manifest.json', url))
    const file = m.ok ? (await m.json())[name] : undefined
    if (!file) return new Response('Not found', { status: 404 })

    const headers = {
      'content-type': 'application/octet-stream',
      'content-disposition': `attachment; filename="${name}"`,
      'content-length': String(file.size),
      'cache-control': 'public, max-age=3600',
    }
    if (request.method === 'HEAD') return new Response(null, { headers })

    const { readable, writable } = new FixedLengthStream(file.size)
    ctx.waitUntil((async () => {
      for (const part of file.parts) {
        const r = await env.ASSETS.fetch(new URL(`/dl/${part}`, url))
        await r.body.pipeTo(writable, { preventClose: true })
      }
      await writable.close()
    })())
    return new Response(readable, { headers })
  },
}
