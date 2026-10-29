import { useState } from 'react'
import { ChevronDown } from 'lucide-react'
import { Container, PageHero, SectionHead } from '@/components/site/Blocks'
import { Reveal } from '@/components/site/Reveal'
import { editions, lines, mb, useHistory, useLatest, type EditionId } from '@/data/updates'

const how = [
  {
    t: 'Quick updates install themselves',
    body: 'With Automatic updates on, a quick update found when BoltOS starts downloads, goes onto the disk and restarts into itself before you have opened anything. One found later waits for the next start.',
  },
  {
    t: 'Big ones wait for you',
    body: 'A big update shows in Settings → Updates and in a note, and installs when you choose Update. Turn Automatic updates off and every update waits for that button.',
  },
  {
    t: 'Nothing half-written',
    body: 'The download has to match its SHA-256. The new kernel is written beside the old one and read back; only then does one sector point the machine at it. A power cut leaves one version or the other, whole.',
  },
  {
    t: 'No restart loops',
    body: 'If a machine restarts for an update and comes back on the old version, it never tries that one by itself again. Update in Settings still offers it.',
  },
]

function EditionCard({ id, name, where, blurb }: { id: EditionId; name: string; where: string; blurb: string }) {
  const latest = useLatest(id)
  const history = useHistory(id)
  const [open, setOpen] = useState(false)
  const older = (history ?? []).filter(h => h.version !== latest?.version)
  return (
    <div className="flex h-full flex-col rounded-3xl border border-line bg-ash p-7 sm:p-9">
      <div className="flex items-center justify-between gap-4">
        <h3 className="text-[24px] font-medium tracking-[-0.03em]">{name}</h3>
        {latest && (
          <span className={'rounded-full px-2.5 py-0.5 font-mono text-[11px] ' + (latest.kind === 'big' ? 'border border-line-strong text-smoke' : 'bg-white text-black')}>
            {latest.kind}
          </span>
        )}
      </div>
      <p className="mt-6 font-mono text-[clamp(2.4rem,5vw,3.4rem)] leading-none tracking-[-0.04em]">
        {latest === undefined ? <span className="text-fog">…</span> : latest ? latest.version : <span className="text-fog">—</span>}
      </p>
      <p className="mt-3 font-mono text-[12.5px] text-fog">
        {latest ? `${latest.date} · ${mb(latest.size)}` : latest === null ? 'No update published yet' : 'Asking boltos.dev'}
      </p>
      {latest && lines(latest.notes).length > 0 && (
        <ul className="mt-7 space-y-2 text-[14.5px] leading-relaxed text-smoke">
          {lines(latest.notes).map(l => (
            <li key={l} className="flex gap-3"><span className="mt-[0.6em] size-1 shrink-0 rounded-full bg-fog" />{l}</li>
          ))}
        </ul>
      )}
      <p className="mt-7 flex-1 text-[14px] leading-relaxed text-fog">{blurb} <span className="text-smoke">{where}.</span></p>
      {older.length > 0 && (
        <div className="mt-7 border-t border-line pt-5">
          <button onClick={() => setOpen(o => !o)} className="flex w-full items-center justify-between text-[13px] text-smoke hover:text-white">
            Earlier versions <ChevronDown className={'size-4 transition-transform ' + (open ? 'rotate-180' : '')} />
          </button>
          {open && (
            <ol className="mt-4 space-y-4">
              {older.map(h => (
                <li key={h.version}>
                  <p className="font-mono text-[12.5px] text-white">{h.version} <span className="text-fog">· {h.date} · {h.kind}</span></p>
                  <p className="mt-1 text-[13px] leading-relaxed text-fog">{lines(h.notes).join(' · ')}</p>
                </li>
              ))}
            </ol>
          )}
        </div>
      )}
    </div>
  )
}

export default function Updates() {
  return (
    <>
      <PageHero
        eyebrow="Updates"
        title={['Always the', <span className="serif-i text-smoke">newest one.</span>]}
        intro="Every edition of BoltOS checks boltos.dev when it starts and every few hours after. These are the versions it finds there right now."
      />
      <Container className="pb-24">
        <div className="grid gap-4 lg:grid-cols-3">
          {editions.map((e, i) => (
            <Reveal key={e.id} delay={i * 0.08}>
              <EditionCard {...e} />
            </Reveal>
          ))}
        </div>
      </Container>
      <section className="border-t border-line py-24 sm:py-32">
        <Container>
          <SectionHead eyebrow="How it works" title={<>Updates, <span className="serif-i text-smoke">by themselves.</span></>}
            intro="A quick update is a small one that is safe to take at any start. A big one changes enough that you choose the moment." />
          <div className="mt-16 grid gap-4 sm:grid-cols-2">
            {how.map((h, i) => (
              <Reveal key={h.t} delay={i * 0.05}>
                <div className="h-full rounded-3xl border border-line p-7 sm:p-9">
                  <p className="font-mono text-[11px] text-fog">0{i + 1}</p>
                  <h3 className="mt-4 text-[22px] font-medium tracking-[-0.03em]">{h.t}</h3>
                  <p className="mt-3 text-[15px] leading-relaxed text-smoke">{h.body}</p>
                </div>
              </Reveal>
            ))}
          </div>
        </Container>
      </section>
    </>
  )
}
