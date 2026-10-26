// boltos.dev is a static site; this script exists for one thing. Workers
// static assets cap a file at 25 MiB, so the BoltOS TV images are stored in
// parts (scripts/downloads.mjs) and served from /dl/<name> here as one
// stream, with the length up front so a browser shows real progress.
// Everything else never reaches this script: wrangler.jsonc runs it first
// only for /dl/*.
export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url)
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
