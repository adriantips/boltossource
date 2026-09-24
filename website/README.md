# boltos.dev

The BoltOS website: React 19, Vite, Tailwind 4 and motion, with a few
components from the react-bits registry (via shadcn) in `src/components/bits/`.

```sh
npm install
npm run dev        # http://localhost:5173
npm run build      # -> dist/, ready for any static host
```

Deployed to boltos.dev as a Cloudflare Worker with static assets
(`wrangler.jsonc`): `npm run build && npx wrangler deploy`. Unknown paths fall
back to `index.html`, which is what lets `/system/network` and friends load
straight from the address bar.

## Content

Everything the pages say lives in `src/data/`:

- `site.ts` -- every outside link and every download. Downloads point at
  GitHub, never at this site, so a new build means changing one URL here.
- `systems.ts` -- the fourteen subsystem pages.
- `apps.ts`, `timeline.ts` -- the app directory and the history.
- `shots.ts` -- captions. `shots.json` is generated, see below.

## Screenshots

Every screenshot is a QEMU screendump of a real build at 1920x1080.
`scripts/shots.py DIR [DIR ...]` converts a folder of PNGs to WebP (full size
plus a 960px thumbnail), crops each windowed capture to its window by diffing
it against `desktop.png`, and rewrites `src/data/shots.json`. A page asks
`hasShot(key)` before drawing an image, so a capture that failed is simply
absent rather than a broken image.
