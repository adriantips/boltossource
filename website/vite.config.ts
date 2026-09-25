import fs from 'node:fs'
import path from 'node:path'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { defineConfig, type Plugin } from 'vite'

// Google gets a sitemap built from the routes in App.tsx plus every
// /system/:slug in systems.ts, so a new subsystem page shows up on its own.
function sitemap(): Plugin {
  return {
    name: 'boltos-sitemap',
    generateBundle() {
      const systems = fs.readFileSync(path.resolve(import.meta.dirname, 'src/data/systems.ts'), 'utf8')
      const slugs = [...systems.matchAll(/^\s+slug: '([a-z0-9-]+)'/gm)].map((m) => m[1])
      const pages = ['', 'system', 'apps', 'gallery', 'timeline', 'creator', 'download', 'docs', ...slugs.map((s) => `system/${s}`)]
      const urls = pages.map((p) => `  <url><loc>https://boltos.dev/${p}</loc></url>`).join('\n')
      this.emitFile({
        type: 'asset',
        fileName: 'sitemap.xml',
        source: `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n${urls}\n</urlset>\n`,
      })
    },
  }
}

export default defineConfig({
  plugins: [react(), tailwindcss(), sitemap()],
  resolve: {
    alias: { '@': path.resolve(import.meta.dirname, './src') },
  },
})
