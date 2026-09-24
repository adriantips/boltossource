import { Container, SectionHead, Button, ArrowLink } from '@/components/site/Blocks'
import { MaskLines, Reveal } from '@/components/site/Reveal'
import { Mark } from '@/components/site/Logo'
import { GitHubIcon } from '@/components/site/GitHubIcon'
import CountUp from '@/components/bits/CountUp'
import DecryptedText from '@/components/bits/DecryptedText'
import { links } from '@/data/site'

const built = [
  ['A boot chain', 'An MBR, a real-mode second stage and a UEFI loader, all taking the CPU to long mode.'],
  ['A kernel', 'Scheduler, processes, syscalls, paging, SMP, ACPI and suspend-to-RAM.'],
  ['Drivers', 'NVMe, AHCI, ATA, xHCI with hubs, HD Audio, AC\'97, e1000, virtio-net and a display adapter.'],
  ['A network stack', 'IPv4, IPv6, TCP, TLS 1.3 with certificate checks, DNS, DHCP and SSH.'],
  ['A desktop', 'A compositor, 43 apps, virtual desktops, tiling, scaling to 4K and screen recording.'],
  ['A browser', 'HTML, a CSS cascade, flex and grid layout, and a JavaScript interpreter.'],
  ['Compilers', 'C, C++ and C# through BoltCC, a native x86-64 backend, an assembler, a linker and make.'],
  ['A test suite', 'Sixty-nine VM tests and fuzzers with their own sanitizer, built because the toolchain had none.'],
]

export default function Creator() {
  return (
    <>
      <section className="relative overflow-hidden pt-40 pb-24 sm:pt-48">
        <div className="grid-bg fade-edges pointer-events-none absolute inset-0 opacity-60" />
        <Container className="relative">
          <Reveal><p className="eyebrow">The creator</p></Reveal>
          <h1 className="display mt-6 text-[clamp(3.2rem,11vw,10.5rem)]">
            <MaskLines lines={['Adrian', 'Derakhshideh.']} />
          </h1>
          <div className="mt-14 grid gap-12 lg:grid-cols-[1fr_1.2fr] lg:items-end">
            <Reveal delay={0.25}>
              <div className="flex items-baseline gap-5">
                <span className="display text-[clamp(6rem,16vw,13rem)] leading-[0.8]">
                  <CountUp to={12} duration={1.6} />
                </span>
                <span className="serif-i text-[clamp(1.6rem,3vw,2.6rem)] text-smoke">years old.</span>
              </div>
            </Reveal>
            <Reveal delay={0.35}>
              <p className="text-[clamp(1.1rem,1.6vw,1.35rem)] leading-relaxed text-smoke">
                Adrian is a twelve-year-old programmer. BoltOS is the operating system Adrian wrote: a 64-bit kernel and everything on top of it,
                written in C and x86-64 assembly without borrowing a bootloader or a kernel from anyone.
              </p>
              <p className="mt-6 font-mono text-[13px] text-fog">
                <DecryptedText text="programmer · systems · x86-64 · C" animateOn="view" sequential speed={30} />
              </p>
            </Reveal>
          </div>
        </Container>
      </section>

      <section className="bg-paper text-black">
        <Container className="py-28 sm:py-40">
          <Reveal>
            <p className="font-mono text-[11px] tracking-[0.18em] text-black/50 uppercase">Why it matters</p>
          </Reveal>
          <h2 className="display mt-8 max-w-5xl text-[clamp(2.2rem,5.5vw,5rem)]">
            <MaskLines lines={['Most people meet these ideas', <>in a <span className="serif-i">university course.</span></>]} />
          </h2>
          <div className="mt-16 grid gap-10 lg:grid-cols-2 lg:gap-20">
            <Reveal>
              <p className="text-[18px] leading-relaxed text-black/70">
                Virtual memory, interrupt routing, TCP retransmission, TLS handshakes, CSS specificity, register allocation for a compiler backend.
                Each of those is a semester somewhere. BoltOS has working code for all of them, and the commit history shows the order they were learned in:
                one subsystem at a time, each fixed until the test that checks it passes.
              </p>
            </Reveal>
            <Reveal delay={0.1}>
              <p className="text-[18px] leading-relaxed text-black/70">
                The documentation is unusually honest for a project of any size. The user guide ends with a list of what BoltOS cannot do yet. The Wi-Fi
                page says there is no radio driver. The vault page lists the two things it does not protect. That habit — say what is true, then go and
                fix it — runs through the whole tree.
              </p>
            </Reveal>
          </div>
        </Container>
      </section>

      <section className="py-24 sm:py-36">
        <Container>
          <SectionHead eyebrow="What Adrian built" title={<>A whole stack, <span className="serif-i text-smoke">top to bottom.</span></>} />
          <div className="mt-16 grid gap-px overflow-hidden rounded-3xl border border-line bg-line sm:grid-cols-2 lg:grid-cols-4">
            {built.map(([k, v], i) => (
              <Reveal key={k} delay={(i % 4) * 0.06} className="bg-black p-7 sm:p-8">
                <p className="font-mono text-[11px] text-fog">{String(i + 1).padStart(2, '0')}</p>
                <h3 className="mt-10 text-[21px] font-medium tracking-[-0.03em]">{k}</h3>
                <p className="mt-2 text-[14px] leading-relaxed text-smoke">{v}</p>
              </Reveal>
            ))}
          </div>
        </Container>
      </section>

      <section className="border-t border-line py-24 sm:py-36">
        <Container>
          <div className="grid gap-px overflow-hidden rounded-3xl border border-line bg-line sm:grid-cols-2 lg:grid-cols-4">
            {[
              [100, 'k', 'lines of C and assembly'],
              [122, '', 'commits in the repository'],
              [32, '', 'commits on the busiest day'],
              [69, '', 'VM tests, run with one command'],
            ].map(([n, s, l]) => (
              <div key={l as string} className="bg-black p-8">
                <div className="display text-[clamp(3rem,6vw,5rem)] tabular-nums">
                  <CountUp to={n as number} duration={2} /><span className="text-fog">{s}</span>
                </div>
                <p className="mt-3 text-[14px] text-smoke">{l}</p>
              </div>
            ))}
          </div>
        </Container>
      </section>

      <section className="pb-28 sm:pb-40">
        <Container className="text-center">
          <Mark className="mx-auto size-14" />
          <h2 className="display mx-auto mt-10 max-w-4xl text-[clamp(2.4rem,6vw,5.5rem)]">
            Follow the work <span className="serif-i text-smoke">as it happens.</span>
          </h2>
          <div className="mt-12 flex flex-wrap justify-center gap-3">
            <Button to={links.repo}><GitHubIcon className="size-4" /> adriantips on GitHub</Button>
            <Button to="/timeline" variant="ghost">Read the timeline</Button>
          </div>
          <div className="mt-10">
            <ArrowLink to={links.roadmap}>What is next: the roadmap</ArrowLink>
          </div>
        </Container>
      </section>
    </>
  )
}
