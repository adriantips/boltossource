// The subsystem pages. Every claim here is taken from the README, the docs or
// the source tree -- if it is not in the repo, it is not on the site.

export type Section = {
  title: string
  body: string[]
  list?: string[]
  code?: { label?: string; text: string }
  shot?: string
  caption?: string
}

export type System = {
  slug: string
  n: string
  title: string
  short: string
  kicker: string
  intro: string
  shot: string
  facts: { k: string; v: string }[]
  sections: Section[]
  source: string[]
}

export const systems: System[] = [
  {
    slug: 'boot',
    n: '01',
    title: 'The boot chain',
    short: 'Boot',
    kicker: 'From the reset vector to long mode, twice over.',
    intro:
      'BoltOS does not borrow a bootloader. A 512-byte MBR, a real-mode second stage and a UEFI application written in C each take the machine from firmware to a 64-bit higher-half kernel, and both hand it exactly the same structure when they get there.',
    shot: 'bootmenu',
    facts: [
      { k: 'Firmware', v: 'Legacy BIOS + UEFI' },
      { k: 'Stage 1', v: '512 bytes, MBR' },
      { k: 'Kernel base', v: '0xFFFFFFFF80100000' },
      { k: 'Boot menu', v: '4 entries, 5 s timeout' },
    ],
    sections: [
      {
        title: 'Legacy BIOS: stage1 and stage2',
        body: [
          'The BIOS loads `stage1.asm` to `0x7C00`. It does one job: read stage2 with INT 13h LBA calls and jump to it at `0x8000`. Stage2 still runs in real mode, and it does everything the kernel should never have to think about -- enable A20, read the E820 memory map, drop into unreal mode to copy the kernel above 1 MiB, build a PML4 with 2 MiB pages and enter long mode.',
          'Stage2 does not have the kernel size compiled into it. It reads a one-sector boot header at LBA 33 to learn where the kernel starts and how long it is. That small decision is what lets a running BoltOS carry a copy of its own bootloader and install itself onto another disk.',
        ],
        code: {
          label: 'boot flow, legacy',
          text: `BIOS ── loads ──▶ stage1.asm (512 B MBR @ 0x7C00)
                      │ INT 13h LBA read
                      ▼
                  stage2.asm (@ 0x8000, real mode)
                      │  boot menu (5 s timeout)
                      │  boot header @ LBA 33 -> kernel LBA + sectors
                      │  A20, E820 map, unreal mode -> copy to 1 MiB
                      │  PML4/PDPT/PD, enter long mode
                      ▼
                  kernel/boot.asm (_start, 64-bit)
                      ▼
                  kmain(bootinfo)`,
        },
      },
      {
        title: 'UEFI: BOOTX64.EFI',
        body: [
          '`boot/uefi_boot.c` is a UEFI application compiled with `clang --target=x86_64-unknown-windows` and linked as an EFI binary. It finds the GOP framebuffer, turns the UEFI memory map into the same E820-style table stage2 produces, builds identity and higher-half page tables and jumps to the kernel at `0x100000`.',
          'The kernel cannot tell which loader started it. Both pass a `bootinfo` struct in `RDI` holding the framebuffer and the memory map, and that is the whole contract.',
        ],
      },
      {
        title: 'A boot menu for bad days',
        body: [
          'Both loaders show the same menu before anything else: **BoltOS**, **Safe mode**, **Recovery shell** and **BoltOS + fsck**. Safe mode skips network, audio and the desktop and drops to the text shell. Recovery also mounts the filesystem read-only. It is the way back in when a setting, a wedged NIC or a crashing app is what is stopping the boot.',
        ],
        shot: 'bootmenu',
        caption: 'The boot menu, drawn by stage2 before the kernel is even loaded.',
      },
      {
        title: 'Proving the kernel arrived',
        body: [
          'A green QEMU boot says very little about real firmware. One real machine booted as far as stage2 and then jumped into a kernel that had never been copied, because its CSM firmware quietly broke unreal mode inside a USB read. The boot header is at version 2 now and carries a sum32 of the kernel image. Stage2 recomputes it at 1 MiB and refuses to jump if they differ.',
          'The kernel is linked at virtual `0xFFFFFFFF80100000` but loaded at physical `0x100000`. `boot.asm` jumps from the identity-mapped entry to its higher-half address and never looks back.',
        ],
      },
    ],
    source: ['boot/stage1.asm', 'boot/stage2.asm', 'boot/uefi_boot.c', 'kernel/boot.asm', 'mkboothdr.py', 'linker.ld'],
  },
  {
    slug: 'kernel',
    n: '02',
    title: 'The kernel',
    short: 'Kernel',
    kicker: 'Processes, threads, syscalls and every core the machine has.',
    intro:
      'A 64-bit higher-half kernel with a preemptive scheduler, ring-3 processes loaded from ELF64, a syscall interface, ACPI, APIC, SMP and suspend-to-RAM. It is written to keep running when the things on top of it do not.',
    shot: 'taskmgr',
    facts: [
      { k: 'Scheduler', v: 'Preemptive round-robin' },
      { k: 'Userland', v: 'Ring 3, ELF64' },
      { k: 'Multiprocessing', v: 'SMP via INIT-SIPI-SIPI' },
      { k: 'Sleep', v: 'ACPI S3, resumes' },
    ],
    sections: [
      {
        title: 'Processes and the scheduler',
        body: [
          '`kernel/sched.c` time-slices threads off the timer; `kernel/proc.c` holds per-process state. Programs are static ELF64 binaries: the loader maps their segments into a fresh address space with a randomised base, and they reach the kernel through `syscall`.',
          'Syscalls run with interrupts enabled, which is less obvious than it sounds. With IF clear, a network wait that parks the CPU with `hlt` until the next tick never gets that tick, and the machine stops dead rather than timing out. Turning interrupts on meant a syscall could be preempted, so the user stack pointer moved from a global onto each process\'s kernel stack, one copy per call.',
        ],
      },
      {
        title: 'A crash in ring 3 is a crash in one program',
        body: [
          'A CPU fault raised by a user program kills that program, not the machine. `isr_handler` checks the saved CS; ring 3 goes to `proc_fault_kill()`, which records the crash, closes descriptors, retires the thread and resumes the next one. Ring 0 still panics, because a kernel fault means kernel state can no longer be trusted.',
          'Every crash is formatted -- registers, page-fault decode, a symbolised backtrace -- and appended to `/var/log/crash.log`, which survives the reboot. The report you need is always from the boot that just failed.',
        ],
        code: { label: 'try it', text: 'crash ring3 null      # fault on purpose\ncrashlog              # read the report back\ncore                  # registers, backtrace, stack' },
      },
      {
        title: 'Zombies, reaping and the watchdog',
        body: [
          'A dying thread cannot free the stack it is standing on or the address space it is running in. So it becomes a zombie, and `schedule()` reaps it on the second switch away -- never the first, because the interrupt frame still lives on the outgoing stack until the epilogue steps off it. Before this, every exited program leaked a 64 KiB stack and its page tables.',
          '`kill` is real, `freeze` and `resume` exist, and a watchdog notices threads that have wedged.',
        ],
      },
      {
        title: 'ACPI, APIC and SMP',
        body: [
          '`kernel/acpi.c` finds the RSDP, walks the RSDT/XSDT and the MADT and validates checksums. The local APIC and I/O APIC replace the legacy PIC, each core gets its own APIC timer, and `kernel/smp.c` wakes the application processors through a trampoline written to low memory.',
        ],
      },
      {
        title: 'Suspend to RAM, and the hard half',
        body: [
          'Shutting down is easy because nothing has to work afterwards. On resume from S3 the CPU is back in 16-bit real mode with paging off and every device at power-on defaults, next to a kernel image that believes nothing happened. `kernel/s3_wake.asm` climbs real → protected → long mode on the page tables that survived in RAM, and then every subsystem runs its own `init()` again.',
          'Four things had to be exactly right and none of them announce themselves: CMOS `0x0F` must read `0xFE`, CR4 comes back with SSE off in a kernel built with `-msse`, reloading GS in long mode zeroes the per-CPU pointer being restored, and PCI configuration space must be back before any driver reads a register. The trampoline prints a letter per stage to serial, because where that line stops is where the machine died.',
        ],
      },
    ],
    source: ['kernel/sched.c', 'kernel/proc.c', 'kernel/syscall.c', 'kernel/elf.c', 'kernel/smp.c', 'kernel/acpi.c', 'kernel/sleep.c'],
  },
  {
    slug: 'memory',
    n: '03',
    title: 'Memory',
    short: 'Memory',
    kicker: 'Pages, heaps and an address space nobody else can read.',
    intro:
      'A physical frame allocator, four-level paging, a kernel heap that gives back what it is given, per-process address spaces with ASLR, SMAP and SMEP, DMA-safe buffers and swap.',
    shot: 'memory',
    facts: [
      { k: 'Paging', v: '4-level, higher half' },
      { k: 'Isolation', v: 'SMAP + SMEP' },
      { k: 'Userland', v: 'ASLR per process' },
      { k: 'Overflow', v: 'Swap' },
    ],
    sections: [
      {
        title: 'Physical and virtual',
        body: [
          '`mm/` holds the physical memory manager, the virtual memory layer, the kernel heap, DMA allocation and swap. The kernel lives in the top 2 GiB with a direct map of physical memory; every process gets its own page-table tree and a randomised load base.',
          'SMEP stops the kernel from ever executing a user page. SMAP stops it from even reading one by accident, so every deliberate copy across the boundary is explicit.',
        ],
      },
      {
        title: 'A heap that gives back what it is given',
        body: [
          'The kernel heap was rewritten to coalesce and return memory rather than slowly fragmenting over a long session. It is also the one piece of the kernel the fuzzers replace: on the host, `kmalloc` becomes a detector with a redzone either side of each block, poison on free, a quarantine for use-after-free, and a switch that makes allocation start failing partway through a parse.',
        ],
      },
      {
        title: 'Stack canaries',
        body: [
          'Kernel and libc are built with `-fstack-protector-strong`. A smashed canary lands in `__stack_chk_fail`, which reports the frame instead of returning into whatever the overflow wrote.',
        ],
      },
    ],
    source: ['mm/', 'kernel/heap.c', 'libc/support.c'],
  },
  {
    slug: 'storage',
    n: '04',
    title: 'Storage and filesystems',
    short: 'Storage',
    kicker: 'Four disk transports, three filesystems, no torn writes.',
    intro:
      'ATA, AHCI SATA, NVMe and USB mass storage sit behind one block layer. BoltFS, the native filesystem, commits atomically with real flush barriers, so the power can go at any instant and the next mount still finds a complete image.',
    shot: 'files',
    facts: [
      { k: 'Transports', v: 'ATA · AHCI · NVMe · USB' },
      { k: 'Filesystems', v: 'BoltFS · FAT32 · EXT2' },
      { k: 'Commits', v: 'Shadow-slot, atomic' },
      { k: 'Recovery', v: 'fsck + mirrored superblock' },
    ],
    sections: [
      {
        title: 'One block layer',
        body: [
          '`kernel/blk.c` is a small registry of block transports plus shared MBR helpers, so the filesystem never learns whether it is talking to a spinning disk or an NVMe drive. The ATA driver tells HDDs from SSDs by IDENTIFY word 217. The NVMe driver creates real queue pairs and takes completions on an MSI/MSI-X interrupt, falling back to polling on controllers with no MSI at all.',
        ],
      },
      {
        title: 'Atomic commits',
        body: [
          'A BoltFS partition holds two blob slots and a mirrored superblock. A commit serialises the tree into the slot that is not live, writes it, flushes the device\'s write cache, and only then swaps the superblock generation. Whatever instant the power fails, the next mount finds the old image or the new one, never a mix. A torn primary superblock fails its checksum and the mirror takes over.',
          'A failed load never triggers a format. The loader builds a detached tree and replaces the live one only after every record succeeds; if nothing loads, the disk is protected read-only and File Explorer says that changes will not be saved.',
        ],
        code: { label: 'shell', text: 'diskinfo           # controllers, disks, what is mounted\nsync               # commit now\nfsck               # check the image\nusb                # USB sticks: list, mount, umount' },
      },
      {
        title: 'A disk you did not ask it to touch',
        body: [
          'Foreign partitions, GPT and hybrid tables, overlapping partitions and unreadable disks are never adopted. Only a small, wholly zero-filled scratch disk is initialised automatically, and every sector is read before that decision. Anything larger goes through the installer, which asks.',
        ],
      },
      {
        title: 'It installs itself',
        body: [
          'The `install` command and the Install BoltOS app write a complete system to another disk from inside a running BoltOS: MBR, stage2, the boot header, the kernel, a FAT32 EFI partition with `BOOTX64.EFI`, and a BoltFS partition seeded from the running tree. The result boots on BIOS and on UEFI. `test/t_install.py` installs onto a blank disk, throws the install medium away and boots the result both ways.',
        ],
        shot: 'installer',
        caption: 'The installer shows the exact layout before it writes anything.',
      },
    ],
    source: ['kernel/blk.c', 'drivers/ata.c', 'drivers/ahci.c', 'drivers/nvme.c', 'fs/', 'kernel/vfs.c', 'kernel/install.c'],
  },
  {
    slug: 'hardware',
    n: '05',
    title: 'Drivers and hardware',
    short: 'Drivers',
    kicker: 'USB hubs, HD Audio codecs, display adapters and the keyboard you actually own.',
    intro:
      'BoltOS drives real hardware, not just what an emulator happens to expose: xHCI USB with hubs and HID, PS/2 input, a PCI display adapter driver with runtime mode setting, Intel HD Audio and AC\'97, and keyboard layouts that know where the @ is.',
    shot: 'devmgr',
    facts: [
      { k: 'USB', v: 'xHCI, hubs, HID, storage' },
      { k: 'Display', v: 'Bochs DISPI, up to 4K' },
      { k: 'Audio', v: 'Intel HDA · AC\'97 · speaker' },
      { k: 'Network', v: 'e1000 · virtio-net' },
    ],
    sections: [
      {
        title: 'USB, and why hubs matter',
        body: [
          'The xHCI driver sets up the device context array, command and event rings and scratchpad buffers, resets each port, addresses each device and walks its descriptors. HID reports then arrive as MSI interrupts instead of being polled.',
          'A Dell keyboard has a hub inside it, as do most monitors and docks, so "plug it into the machine instead" is not a workaround -- the hub travels with the keyboard. Enumeration is a path of route strings rather than a port number, and full-speed devices behind a high-speed hub get the transaction-translator fields they need. Without them the keyboard addresses successfully and then answers nothing, forever.',
        ],
      },
      {
        title: 'PORTSC is not a register you read-modify-write',
        body: [
          'A real keyboard stayed dead through four rounds of fixes. The cause was one line: bit 1 of PORTSC reads 1 on an enabled port, and writing 1 back disables it. A port reset that read the register, OR\'d in the reset bit and wrote it back was switching off exactly the ports with something plugged in. The symptom was "N ports with something attached, 0 devices".',
        ],
      },
      {
        title: 'Display',
        body: [
          '`drivers/gpu.c` finds the PCI display controller, identifies it from its vendor and device ID -- QEMU, VirtualBox, VMware SVGA II, VirtIO-GPU, Cirrus, Intel, NVIDIA, AMD -- sizes the VRAM aperture from BAR0 and uses the Bochs DISPI interface to change mode at runtime, from 1024×768 to 4K. There is one wallpaper render per resolution, so nothing is ever scaled at boot.',
        ],
      },
      {
        title: 'Keyboards in more than one language',
        body: [
          'Layouts are tables with every level of every key, switchable with `kbd`. A German keyboard needs AltGr+Q for the @ sign, and for a while it simply could not type one. `kbd show` prints every key of every level, which is how that was found.',
        ],
      },
    ],
    source: ['drivers/xhci.c', 'drivers/gpu.c', 'drivers/framebuffer.c', 'drivers/hda.c', 'drivers/ac97.c', 'drivers/keyboard.c', 'kernel/pci.c'],
  },
  {
    slug: 'network',
    n: '06',
    title: 'Networking',
    short: 'Network',
    kicker: 'A dual-stack TCP/IP stack with TLS 1.3, built one RFC at a time.',
    intro:
      'Ethernet to HTTPS with nothing borrowed: ARP, IPv4 and IPv6, ICMP, UDP, TCP, DHCP, DNS with a cache, SNTP, a firewall, TLS 1.2 and 1.3 with real certificate verification, BSD sockets in ring 3, and SSH in both directions.',
    shot: 'network',
    facts: [
      { k: 'Families', v: 'IPv4 + IPv6 (SLAAC)' },
      { k: 'TLS', v: '1.2 and 1.3' },
      { k: 'Certificates', v: 'X.509, RSA + ECDSA' },
      { k: 'Remote', v: 'SSH-2 server + client' },
    ],
    sections: [
      {
        title: 'The stack',
        body: [
          'Frames arrive on an e1000 or virtio-net NIC. Above them: ARP and Neighbour Discovery, IPv4 and IPv6 with SLAAC, ICMP, UDP and TCP. One connection type serves both families, so a program never has to care which one carried it. DHCP configures the interface at boot; `ifconfig` changes are written to `/etc` and survive the reboot, along with `/etc/hosts` and a DNS cache.',
        ],
      },
      {
        title: 'TLS that checks who it is talking to',
        body: [
          '`net/tls.c` speaks TLS 1.2 and 1.3 with ECDHE over X25519 and P-256, and AES-128-GCM and AES-256-GCM. The certificate chain is verified -- RSA PKCS#1 v1.5 and PSS, ECDSA P-256, hostname and validity dates -- against a runtime trust store, so it holds up against an active man-in-the-middle and not only a passive listener. X25519 is checked against RFC 7748\'s own test vectors.',
        ],
      },
      {
        title: 'Sockets, servers and SSH',
        body: [
          'Ring-3 programs get BSD-style sockets and can accept connections. `serve` is an HTTP file server over real accepted connections. `sshd` is an SSH-2 server with password authentication and an encrypted channel to a real shell, so the machine can be administered and developed on from another computer -- and it can SSH out, too.',
        ],
        code: { label: 'shell', text: 'netinfo                      # addresses, routes, DNS\nping example.com\ndownload https://example.com/file.mp3\nsshd start                   # then: ssh user@<ip>' },
      },
      {
        title: 'Wi-Fi, stated honestly',
        body: [
          '802.11 association, WPA2-PSK key derivation, CCMP and DHCP over the air are all implemented, and the WPA key schedule is diffed against Python\'s own `hashlib` and `hmac`. What is missing is a driver for any specific radio chip; the stack is ready for one. The Wi-Fi picker and the `wifi` command say so rather than pretending.',
        ],
      },
    ],
    source: ['net/tcp.c', 'net/ip.c', 'net/tls.c', 'net/x509.c', 'net/dhcp.c', 'net/dns.c', 'kernel/ssh.c'],
  },
  {
    slug: 'desktop',
    n: '07',
    title: 'The desktop',
    short: 'Desktop',
    kicker: 'A compositing window manager in liquid glass, drawn one pixel at a time.',
    intro:
      'Windows that open, minimise and tile with an animation; a floating dock, a tray and a desktop switcher in frosted glass; Start with search; Quick Settings and a notification centre with a calendar; four virtual desktops, display scaling to 4K, screenshots, screen recording and a clipboard that carries pictures. All of it redrawn from scratch in October 2026.',
    shot: 'desktop',
    facts: [
      { k: 'Apps', v: '43' },
      { k: 'Desktops', v: '4 virtual' },
      { k: 'Scaling', v: '100 – 200 %' },
      { k: 'Type', v: 'Inter + DejaVu Sans Mono' },
    ],
    sections: [
      {
        title: 'A compositor with no GPU to lean on',
        body: [
          'Every window renders into a surface of its own, and the compositor builds each frame from damage: only the rectangles that changed are recomposited and presented. That is what lets a window open, close, minimise and tile with an animation without its app drawing a single extra frame, and why moving the mouse costs two 32×32 copies instead of a recomposite. Each app is still one `kernel/app_*.c` file exposing draw, key, click, drag and tick callbacks.',
        ],
        shot: 'startmenu',
        caption: 'Press Win and type. A best match first, then apps, then your files.',
      },
      {
        title: 'Glass that is cheap enough to use',
        body: [
          'The dock, the tray, Start, Quick Settings and the notification centre are frosted glass over whatever is behind them. The blurred backdrop is captured once -- a box filter that sums red and blue packed into one word -- stretched back to full size once, and every frame after that copies rows out of it. Under QEMU without acceleration a 640×620 panel now draws in 20 ms; it used to take 157.',
          '`display bench` times every primitive inside the running machine, and `display` reports frame times and the worst frame since you last asked. That is how the glass got fast enough to keep.',
        ],
        shot: 'quicksettings',
        caption: 'Quick Settings hangs off the tray: tiles, brightness, volume, and the Mixer.',
      },
      {
        title: 'Real type, real icons',
        body: [
          'Interface text is Inter and the terminal is DejaVu Sans Mono. `kernel/font.c` rasterises each glyph from its outline by accumulating the exact area every edge covers in every pixel -- the font-rs method -- and kerns it, where the older `kernel/ttf.c` supersampled 4×4 and smeared. Inter is a variable font; `mkuifont.py` instances it at build time, because applying gvar deltas is not a kernel\'s job.',
          'App icons are drawn as SVG in `mkicons.py` and rendered into atlases at 20, 40 and 48 pixels: the sizes the switcher, the dock and Start draw them, so none is scaled on the way to the screen. Under emulation eighteen scaled dock icons cost 23 ms against 8 for native ones.',
        ],
      },
      {
        title: 'Snapping, tiling and four desktops',
        body: [
          'Drag a title bar to an edge for a half, to a corner for a quarter, to the top to maximise. The landing zone is drawn under the window while you hold the button. Win+arrow walks between zones from the keyboard. One function computes every zone\'s rectangle, which is why the preview and the placement agree -- they used to disagree by a pixel on odd widths.',
          'A window on another desktop is not just unpainted: it is out of the dock, out of Alt+Tab and out of every hit test, so nothing you click or type can reach a window you cannot see.',
        ],
      },
      {
        title: 'Scaling to 4K without rewriting 43 apps',
        body: [
          'Apps place text at literal pixel coordinates. So `display scale 200%` makes the desktop smaller -- 1920×1080 logical on a 3840×2160 panel -- and the compositor blows the frame back up. Every app doubles in size without being told. 125% and 150% are offered too, and the Settings page says honestly that they are not pixel-exact.',
        ],
      },
      {
        title: 'Screenshots and screen recording',
        body: [
          'F12 captures the panel to a PNG through a real encoder: per-row filter selection, LZ77 over a 32 KiB window, fixed-Huffman deflate. A 1024×768 desktop is about 20 KB. Ctrl+F12 records an animated PNG, writing only the changed rectangle of each frame and nothing at all for frames nobody could tell apart. The encoder is tested by decoding its output with Python\'s zlib, because an encoder only ever read by its own decoder can be wrong in matching pairs.',
        ],
      },
      {
        title: 'Animations, and an honest Auto',
        body: [
          'Windows grow open, fly to the dock when you minimise them and slide between desktops; panels fade in. On a machine that draws slowly that is a slideshow, so Auto times a frame copy and an alpha fill at boot and turns motion off below a line. Under QEMU TCG the copy takes 7.1 ms; under WHPX, 0.3. Settings and `display motion auto|full|reduced` override it, and `display` says which it chose and why.',
        ],
      },
      {
        title: 'Shortcuts are data, not code',
        body: [
          'Every global shortcut is a row in `kernel/keymap.c`. `keys lock Ctrl+Alt+K` rebinds one; changes live in `/etc/keys.conf`. A bare letter is refused, because binding `k` alone would take that letter away from every program.',
        ],
      },
    ],
    source: ['kernel/gui.c', 'kernel/gui_shell.c', 'kernel/gfx.c', 'kernel/font.c', 'kernel/ui.c', 'mkicons.py', 'kernel/keymap.c', 'kernel/png_enc.c', 'kernel/app_*.c'],
  },
  {
    slug: 'web',
    n: '08',
    title: 'Browser and BoltJS',
    short: 'Browser',
    kicker: 'An HTML engine, a CSS layout engine and a JavaScript interpreter.',
    intro:
      'The Browser parses real HTML into a DOM, runs a CSS cascade with specificity and inheritance, lays out boxes with flexbox and grid, and runs page scripts in BoltJS, a from-scratch interpreter with an event loop, Promises and fetch().',
    shot: 'browser',
    facts: [
      { k: 'Layout', v: 'Block, inline, flex, grid' },
      { k: 'Script', v: 'BoltJS, Promises, fetch()' },
      { k: 'Transport', v: 'HTTP/1.1 + TLS 1.3' },
      { k: 'Runs in', v: 'Ring 3' },
    ],
    sections: [
      {
        title: 'DOM and CSS',
        body: [
          '`kernel/dom.c` is a tokenizer and tree builder with implicit-close rules and void, self-closing and raw-text handling, plus a selector engine backing `querySelector`. `kernel/layout.c` resolves colours in every common syntax, computes specificity and source order, inherits properties, and lays out the box model, inline text with word wrap, flexbox (`flex-grow`, `gap`, `justify-content`, `align-items`) and CSS grid with `fr` tracks and `repeat()`.',
        ],
      },
      {
        title: 'BoltJS',
        body: [
          'A lexer, a precedence-climbing parser and an AST walker. It is wired to the real DOM -- `createElement`, `appendChild`, attributes, `document.cookie`, `localStorage` -- and mutations reflow the page. A VM lives as long as the page does, so there is an event loop: `addEventListener`, `setTimeout`, `setInterval`, `requestAnimationFrame`, and `fetch()` returning real Promises drained through a microtask queue. There is no FPU in the kernel, so numbers are 64-bit integers.',
        ],
        code: { label: 'runs on BoltOS', text: `fetch("https://example.com/data.json")
  .then(r => r.json())
  .then(d => {
    const li = document.createElement("li");
    li.className = "row";
    li.textContent = d.title;
    document.querySelector("#list").appendChild(li);
  })
  .catch(e => console.log("no network: " + e));` },
      },
      {
        title: 'Out of the kernel',
        body: [
          'The same engine sources are also compiled into `/bin/browser` and `/bin/js`, which run in ring 3. The kernel exposes only four privileged primitives -- map the framebuffer, read a key, fetch a URL, hand the screen back -- and no HTML parsing, layout or script execution happens in ring 0.',
        ],
      },
      {
        title: 'OldBrowser, a NetSurf port',
        body: [
          'OldBrowser ports the architecture of NetSurf, the small portable C browser: its URL object, content cache, `browser_window` state machine and framebuffer widget toolkit, driving libhubbub for HTML and libcss for selectors, with BoltOS\'s own image decoders and HTTP stack underneath.',
        ],
        shot: 'legacy_nssite',
        caption: 'An early OldBrowser render, June 2026.',
      },
    ],
    source: ['kernel/dom.c', 'kernel/layout.c', 'kernel/js.c', 'user/browser_main.c', 'oldbrowser/'],
  },
  {
    slug: 'toolchain',
    n: '09',
    title: 'A development platform',
    short: 'Toolchain',
    kicker: 'Compilers, an assembler, a linker, make, a debugger and an IDE -- all on the machine.',
    intro:
      'You can write software on BoltOS without leaving it. BoltCC compiles C, C++ and C#; a native backend emits x86-64 through an assembler that is checked against NASM; `ld`, `ar` and `make` build real programs; a source-level debugger and core dumps find out why they broke.',
    shot: 'ide',
    facts: [
      { k: 'Languages', v: 'C · C++ · C# · Python · JS' },
      { k: 'Native code', v: 'cc -N → ELF64' },
      { k: 'Build', v: 'as · ld · ar · make' },
      { k: 'History', v: 'vcs, on the machine' },
    ],
    sections: [
      {
        title: 'BoltCC',
        body: [
          'One pipeline for three dialects: lexer, recursive-descent parser, AST, stack bytecode and a VM. A C preprocessor gives it `#include` and `#define`, which is what makes multi-file programs possible. It has structs, arrays, recursion and forward references; C gets real `printf` formatting, C++ gets `std::cout` chains, C# gets `Console.WriteLine` with `{0}` placeholders.',
          '`cc -N` is a second backend that emits x86-64, assembles it and links a native ELF64. `cc -S` prints the assembly, `cc -c` writes an object file, and `cc -g` runs the program under a source-level debugger.',
        ],
        code: { label: 'on BoltOS', text: 'cc -N hello.c              # native ELF64\nrun ./hello\ncc -S hello.c              # show me the x86-64\ncc -g buggy.c              # step through it' },
      },
      {
        title: 'The assembler, checked against NASM',
        body: [
          '`as` assembles NASM-syntax x86-64 and writes relocatable objects with `-c`. `test/t_asm.py` diffs every instruction encoding it produces against the real NASM on the host, byte for byte. `ld` links objects and System V `ar` archives into ELF64 programs -- and a megabyte of `.bss` is no longer a megabyte of zeros in the file.',
          '`make` understands rules, variables, pattern rules and `.PHONY`, and rebuilds on a prerequisite that *differs*, not one that is merely newer.',
        ],
      },
      {
        title: 'Code, the IDE',
        body: [
          'A project tree down the left, tabs across the top, an editor with line numbers, auto-indent, bracket pairing and live syntax highlighting, and a console pane where Run streams the program\'s output. Projects have targets defined in `build.txt`, and `vcs` keeps their history -- init, add, commit, log, diff, checkout, branch -- on the machine that has the project.',
        ],
        shot: 'ide',
      },
      {
        title: 'BoltShell',
        body: [
          'The shell is a language: variables, conditions, loops and functions, `test` and `[`, `source`, and a manual that answers -- `man` has pages, search and a coverage report. `edit` is a full-screen text editor for the terminal. There are over 150 commands.',
        ],
        shot: 'terminal_sysinfo',
      },
      {
        title: 'Python and JavaScript',
        body: [
          'BoltPy is a Python-3 subset interpreter behind the `python` command and the IDE\'s Python tab. BoltJS runs scripts with `js FILE.js` in ring 3. BoltRT goes one step further: `compile` packs your source into a PE32+ stub and produces a real `.exe` that runs on Windows 11 and under BoltOS.',
        ],
      },
    ],
    source: ['kernel/boltcc.c', 'kernel/boltpy.c', 'kernel/app_ide.c', 'kernel/cmd_as.c', 'kernel/vcs.c', 'user/boltrt.c'],
  },
  {
    slug: 'security',
    n: '10',
    title: 'Security and accounts',
    short: 'Security',
    kicker: 'Real accounts, enforced permissions, and a vault that is actually encrypted.',
    intro:
      'Multi-user from the file system up: salted PBKDF2 password hashes, POSIX-style ownership checked in one place for the shell, the VFS and the GUI alike, scoped sudo, a lock screen, a firewall, and an AES-256-GCM encrypted vault.',
    shot: 'security',
    facts: [
      { k: 'Passwords', v: 'PBKDF2-HMAC-SHA256' },
      { k: 'Vault', v: 'AES-256-GCM' },
      { k: 'Privilege', v: 'sudo, one command' },
      { k: 'Kernel', v: 'SMAP · SMEP · ASLR · canaries' },
    ],
    sections: [
      {
        title: 'Passwords that are hashed, not checksummed',
        body: [
          'Accounts live in `/etc/passwd` with a per-account random salt and 4096 rounds of PBKDF2-HMAC-SHA256, compared in constant time. A missing account is hashed anyway before being rejected, so the table cannot be enumerated by timing, and repeated failures back off exponentially at the shell and at the sign-in screen.',
        ],
        shot: 'login',
        caption: 'The sign-in screen.',
      },
      {
        title: 'One function answers every permission question',
        body: [
          'Every file carries a uid, a gid and mode bits, persisted in the BoltFS image. `fs_may()` is the single place those are checked, and the shell, the VFS and the GUI all go through it -- so a ring-3 program cannot open what `cat` would refuse. `sudo` elevates exactly one command and drops the privilege afterwards.',
        ],
      },
      {
        title: 'The vault',
        body: [
          '`vault` is an encrypted directory. Locked, its files are one AES-256-GCM sealed archive and the directory is empty. The key comes from PBKDF2 with 16384 rounds and lives in kernel memory only while the vault is open, so locking the screen, logging out and shutting down all seal it.',
          'Locking commits twice more, because BoltFS alternates between two slots and the previous image still has the plaintext in it. `test/t_vault.py` searches the raw disk images afterwards: neither file is anywhere on any of them. The docs also say plainly what the vault does not protect, because a vault that is believed to do more is worse than none.',
        ],
      },
    ],
    source: ['kernel/users.c', 'kernel/vault.c', 'net/firewall.c', 'kernel/app_security.c'],
  },
  {
    slug: 'media',
    n: '11',
    title: 'Audio and media',
    short: 'Audio',
    kicker: 'An HD Audio codec graph, walked by hand, and an MP3 decoder checked against mpg123.',
    intro:
      'Intel HD Audio, AC\'97 and the PC speaker behind one 48 kHz interface; a mixer with real mutes; microphone capture; WAV and a from-scratch MPEG Layer III decoder; and a Media player that does not care which one it opened.',
    shot: 'media',
    facts: [
      { k: 'Codecs', v: 'HDA · AC\'97 · PC speaker' },
      { k: 'Formats', v: 'WAV · MP3' },
      { k: 'Stream', v: '48 kHz / 16-bit stereo' },
      { k: 'Input', v: 'Microphone capture' },
    ],
    sections: [
      {
        title: 'Driving HD Audio',
        body: [
          'An HDA codec arrives as a graph of widgets with nothing connected. The driver walks it, scores output pins from their configuration defaults, traces a route back to a converter through mixers and selectors, picks that branch at every fork, unmutes every amplifier on the way and turns the pin on. Volume is a real gain stage on that route, not a software multiply.',
        ],
      },
      {
        title: 'An MP3 decoder, sample-for-sample',
        body: [
          '`kernel/mp3.c` handles MPEG-1, 2 and 2.5 Layer III: the bit reservoir, Huffman decoding over the standard\'s 32 codebooks, requantisation, mid/side and intensity stereo, the IMDCTs and the polyphase synthesis filterbank. The standard\'s constant tables were read out of existing binaries and proved -- every codebook must be a complete prefix code -- because a mistyped codeword shows up as noise, never as an error.',
          'The test encodes with LAME, decodes with mpg123 and with BoltOS, and diffs the two PCM streams sample by sample.',
        ],
      },
      {
        title: 'The mixer',
        body: [
          'Per-channel levels and mutes that remember where you had them, kept in `/etc/mixer.conf`. "Mute" used to be a volume change that forgot the old level.',
        ],
        shot: 'mixer',
      },
    ],
    source: ['drivers/hda.c', 'drivers/ac97.c', 'kernel/mp3.c', 'kernel/wav.c', 'kernel/app_media.c'],
  },
  {
    slug: 'software',
    n: '12',
    title: 'Packages, SDK and Windows programs',
    short: 'Software',
    kicker: 'Software the kernel has never heard of, installed and run anyway.',
    intro:
      'A package format and repository format, a package manager that installs over HTTP with dependencies, a Software store for people who never open a terminal, a documented syscall ABI and SDK for building programs on Windows, and a loader for real PE32+ Windows executables.',
    shot: 'software',
    facts: [
      { k: 'Packages', v: 'BOLTPKG 1, over HTTP' },
      { k: 'ABI', v: 'Syscall ABI v1, ELF64' },
      { k: 'SDK', v: 'bolt new / build / ship' },
      { k: 'Windows', v: 'PE32+ console .exe' },
    ],
    sections: [
      {
        title: 'A real package manager',
        body: [
          'A package is a text header, a blank line, and its files. Repositories are directories or `http://` URLs. `pkg` installs, removes, runs, builds and publishes, resolving `requires` lines as dependencies. The Software app is the same thing with buttons.',
        ],
        code: { label: 'a package', text: `BOLTPKG 1
package cproject
version 1.0
kind    source
summary a starter multi-file C project
requires boltlib mathx

file 41 /home/Documents/newproj/build.txt
<41 raw bytes>` },
      },
      {
        title: 'The SDK',
        body: [
          '`SDK.md` documents the static ELF64 format, syscall ABI version 1, the public headers and the ring-3 C runtime. On Windows, `sdk\\bolt` covers the round trip: `bolt new` starts a console or full-screen project, `bolt build` links it, and `bolt ship` packages it and serves it as a repository the guest installs from with `pkg install`.',
          'boltcraft, the voxel sandbox, is built exactly this way. The kernel links not one line of it.',
        ],
      },
      {
        title: 'Windows executables',
        body: [
          '`kernel/pe.c` parses PE32+ headers, maps sections by RVA, binds the import table against an in-kernel kernel32 shim using the Microsoft x64 calling convention, and calls the entry point. `winrun app.exe` runs a real Windows console program.',
        ],
      },
    ],
    source: ['kernel/pkg.c', 'include/pkg.h', 'sdk/', 'SDK.md', 'kernel/pe.c', 'user/winhello.c'],
  },
  {
    slug: 'games',
    n: '13',
    title: 'Games',
    short: 'Games',
    kicker: 'DOOM, a defusal shooter, a voxel sandbox and a Game Boy.',
    intro:
      'Integer-only 3D on a kernel with the FPU switched off. DOOM runs from source. BoltStrike is an original round-based shooter whose every texture and sound is generated at startup. boltcraft is a voxel world built with the public SDK. And there is a Game Boy emulator, because why not.',
    shot: 'doom_play',
    facts: [
      { k: 'DOOM', v: 'E1M1, playable' },
      { k: 'BoltStrike', v: '≈ 2,300 lines, 0 assets' },
      { k: 'boltcraft', v: '256×256×64 blocks' },
      { k: 'Game Boy', v: 'SM83 · PPU · APU · MBC1–5' },
    ],
    sections: [
      {
        title: 'DOOM',
        body: [
          'The doomgeneric port is built against a tiny freestanding libc shim and a BoltOS platform layer, with the shareware WAD embedded in the kernel image. The DOOM app pumps the engine, routes the keyboard and blits its 640×400 frame into a window.',
        ],
        shot: 'doom',
      },
      {
        title: 'BoltStrike',
        body: [
          'Two bombsites, five bots a side, a forty-second fuse. The map is original geometry in a familiar shape, and there is no `.bsp`, no `.wad` and no `.wav` anywhere: every wall, soldier, gun and sound comes out of `st_tex.c` and `st_sfx.c` at startup. The whole game adds under a megabyte to the kernel.',
        ],
        shot: 'strike_play',
      },
      {
        title: 'boltcraft',
        body: [
          'Dig, build, swim, get lost in a cave, watch the sun go down. 256×256×64 blocks, no chunk loader, no GPU, about 90 KiB. It is a third-party program in the strict sense: it talks to the kernel only through the eleven syscalls in the SDK.',
        ],
        shot: 'boltcraft_shot',
      },
      {
        title: 'Game Boy',
        body: [
          'An SM83 CPU core, a scanline PPU, four sound channels, the divider and timer, and MBC1, 2, 3 and 5 cartridges in about 1,500 lines. Cartridge RAM is written back next to the ROM every few seconds and whenever the window loses focus. No ROMs are shipped.',
        ],
        shot: 'gameboy',
        caption: 'With no cartridge in /home/user/roms, it says so.',
      },
      {
        title: 'And the small ones',
        body: ['Solitaire with suit-locked foundations and a test that plays it, Minesweeper, 2048, Snake, Tic-Tac-Toe, Game of Life, a Piano and a Matrix rain.'],
        shot: 'solitaire',
      },
    ],
    source: ['doom/', 'strike/', 'boltcraft/', 'gb/', 'kernel/app_solitaire.c'],
  },
  {
    slug: 'quality',
    n: '14',
    title: 'Testing and fuzzing',
    short: 'Testing',
    kicker: 'Sixty-nine tests that boot the real image, and fuzzers that found eleven bugs.',
    intro:
      'The test suite boots the actual disk image in QEMU and drives it over serial and QMP -- types into it, clicks it, pulls its power. Where an outside answer exists, the test asks for it: NASM for the assembler, mpg123 for MP3, Python\'s zlib for PNG, hashlib for WPA.',
    shot: 'terminal_ps',
    facts: [
      { k: 'VM tests', v: '69' },
      { k: 'Fuzz targets', v: '37, in ~2 min' },
      { k: 'Defects found', v: '11 by fuzzing' },
      { k: 'Full suite', v: '≈ 40 minutes' },
    ],
    sections: [
      {
        title: 'BoltVM',
        body: [
          '`test/boltvm.py` boots one QEMU machine with a serial log and a QMP control channel. It can sign in, open apps from the Start menu, type, click, drag, take screenshots, warm-reset and yank the power. Persistence tests pull the plug mid-write and check that what comes back is the old file or the new one, never half of each.',
        ],
      },
      {
        title: 'External oracles',
        body: [
          'A test that can only assert "it did not crash" is a weak test. So `t_asm.py` diffs instruction encodings against NASM, `t_mp3.py` diffs PCM against mpg123, `t_shot.py` decodes PNGs with Python\'s zlib and checks exact pixels, and `t_wifi.py` checks the WPA key schedule against `hashlib` -- which caught two expected values typed in wrong while the C was right.',
        ],
      },
      {
        title: 'Fuzzing the parts a stranger writes',
        body: [
          'The network stack, the TLS and X.509 parsers, the HTML, CSS and JS engines, and the image and audio decoders all parse bytes somebody else chose, in ring 0. `test/t_fuzz.py` builds five harnesses over those sources and runs 37 targets. There is no libFuzzer or AddressSanitizer on this toolchain, so the repo carries both halves: a mutator that starts from inputs that parsed once, and a heap with redzones, poison, a quarantine and failure injection.',
          'Each harness proves itself on a known input before it fuzzes anything. One had its DNS stub\'s return convention inverted, so every HTTP target exited before parsing a byte and reported a clean run at two million inputs a second. A broken fuzzer looks exactly like one that found nothing.',
        ],
        code: { label: 'host', text: 'python test/t_fuzz.py                   # ~1 minute\npython test/t_fuzz.py --iters 200000    # a soak\npython test/run_all.py                  # everything, ~40 minutes' },
      },
    ],
    source: ['test/boltvm.py', 'test/run_all.py', 'test/t_fuzz.py', 'test/host/'],
  },
]

export const systemBySlug = Object.fromEntries(systems.map(s => [s.slug, s]))
