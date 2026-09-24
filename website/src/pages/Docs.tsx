import { ArrowUpRight, BookOpen, Code2, Map, SquareTerminal, Package, HardDrive, Globe } from 'lucide-react'
import { Container, PageHero, SectionHead, CodeBlock } from '@/components/site/Blocks'
import { Reveal } from '@/components/site/Reveal'
import { links } from '@/data/site'

const docs = [
  { icon: BookOpen, t: 'Using BoltOS', f: 'GUIDE.md', href: links.guide, d: 'Installing, signing in, the desktop, files, networks, accounts, shutting down, troubleshooting — and an honest list of what it cannot do yet.' },
  { icon: Code2, t: 'Developing on BoltOS', f: 'DEVELOPING.md', href: links.developing, d: 'The compilers, the assembler and linker, make, projects, the debugger and core dumps, version control.' },
  { icon: Package, t: 'The SDK', f: 'SDK.md', href: links.sdk, d: 'The ELF64 format, syscall ABI version 1, the public headers, and building programs on another machine.' },
  { icon: SquareTerminal, t: 'BoltShell', f: 'SHELL.md', href: links.shell, d: 'Variables, conditions, loops, functions and quoting in the shell.' },
  { icon: Map, t: 'Roadmap', f: 'ROADMAP.md', href: links.roadmap, d: 'A hundred features between here and daily-drivable, in seven phases.' },
  { icon: HardDrive, t: 'Booting from USB', f: 'TUTORIALBOOTUSB.MD', href: links.blob('TUTORIALBOOTUSB.MD'), d: 'Writing boltusb.img to a stick and booting it on a real machine, end to end.' },
  { icon: Globe, t: 'README', f: 'README.md', href: links.readme, d: 'Nearly three thousand lines on how every subsystem works and why.' },
]

const commands = [
  ['Files', 'ls -l · tree · cd · cp · mv · rm · find · grep · diff · edit · chmod · chown · trash · recover'],
  ['System', 'sysinfo · cpuinfo · meminfo · diskinfo · devices · sensors · uptime · dmesg · crashlog · core · logs'],
  ['Processes', 'ps · top · kill · freeze · resume · jobs · priority · services · monitor'],
  ['Accounts', 'whoami · id · users · useradd · passwd · sudo · lock · logout · vault'],
  ['Network', 'netinfo · ifconfig · ping · trace · dns · hosts · ntp · download · browse · webx · wifi · firewall · serve · sshd'],
  ['Develop', 'cc · as · ld · ar · make · build · vcs · python · js · compile · winrun · run · pkg'],
  ['Desktop', 'screenshot · rec · desk · snap · display · keys · kbd · notify · clip'],
  ['Audio', 'play · media · mixer · mic'],
  ['Power', 'poweroff · reboot · suspend · power status · install · fsck · sync'],
]

export default function Docs() {
  return (
    <>
      <PageHero
        eyebrow="Documentation"
        title={['Read the', <span className="serif-i text-smoke">manual.</span>]}
        intro={<>BoltOS documents itself in the repository, next to the code it describes. On the machine, <code className="inline-code">man</code> answers the same questions offline.</>}
      />
      <Container className="pb-24">
        <div className="grid gap-px overflow-hidden rounded-3xl border border-line bg-line sm:grid-cols-2 lg:grid-cols-3">
          {docs.map((d, i) => (
            <Reveal key={d.f} delay={(i % 3) * 0.06} className="bg-black">
              <a href={d.href} target="_blank" rel="noreferrer" className="group flex h-full flex-col p-8 transition-colors hover:bg-white/[0.03]">
                <div className="flex items-center justify-between">
                  <d.icon className="size-6" strokeWidth={1.4} />
                  <ArrowUpRight className="size-4 text-fog transition-all group-hover:-translate-y-0.5 group-hover:translate-x-0.5 group-hover:text-white" />
                </div>
                <h3 className="mt-12 text-[22px] font-medium tracking-[-0.03em]">{d.t}</h3>
                <p className="mt-1 font-mono text-[12px] text-fog">{d.f}</p>
                <p className="mt-4 text-[14.5px] leading-relaxed text-smoke">{d.d}</p>
              </a>
            </Reveal>
          ))}
        </div>
      </Container>

      <section className="border-t border-line py-24 sm:py-32">
        <Container>
          <SectionHead eyebrow="Quick start" title={<>First ten minutes.</>} />
          <div className="mt-14 grid gap-4 lg:grid-cols-2">
            <CodeBlock label="find your way around" text={'help                 # every command, by group\nhelp cc              # one command\nman display          # the manual page\nman -k network       # search the manual\nsysinfo              # what am I running on?'} />
            <CodeBlock label="write and run a C program" text={'cd /home/user\nedit hello.c                 # full-screen editor\ncc hello.c                   # compile and run\ncc -N hello.c                # a native ELF64\nrun ./hello'} />
            <CodeBlock label="get online" text={'netinfo                      # DHCP should have done this\nping example.com\nbrowse https://example.com   # a page, as text\nwebx https://example.com     # the ring-3 browser'} />
            <CodeBlock label="keep your work" text={'install                      # list disks and the planned layout\ninstall nvme0n1              # write BoltOS to a disk\nsync                         # commit the filesystem now\npoweroff                     # commits, marks clean, S5'} />
          </div>
        </Container>
      </section>

      <section className="border-t border-line py-24 sm:py-32">
        <Container>
          <SectionHead eyebrow="Commands" title={<>Over 150 of them.</>} intro={<>A selection, grouped the way <code className="inline-code">help</code> groups them.</>} />
          <div className="mt-14 divide-y divide-line border-y border-line">
            {commands.map(([g, c]) => (
              <Reveal key={g} y={10} className="grid gap-3 py-5 sm:grid-cols-[10rem_1fr]">
                <p className="text-[15px] font-medium">{g}</p>
                <p className="font-mono text-[13px] leading-relaxed text-smoke">{c}</p>
              </Reveal>
            ))}
          </div>
        </Container>
      </section>
    </>
  )
}
