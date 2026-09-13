# BoltOS roadmap — 100 features to daily-drivable and dev-ready

Two bars everything here serves:

- **Daily-drivable** — a non-developer installs it, boots it, logs in, gets on a
  network, browses the real web, writes a note, plays audio and shuts down
  safely, every day, without data loss or babysitting, and it survives a crash
  or power cut.
- **Development-ready** — a developer writes, builds, runs, debugs and versions
  real multi-file software *on BoltOS itself*, and installs third-party tools
  through the package manager.

Nothing on this list is here to pad a feature count. Each item is something the
OS cannot currently do that one of those two bars requires.

Status: `[x]` shipped · `[~]` in progress · `[ ]` not started · `[!]` blocked,
with the reason given

## Phase 0 — Boot & install

- [x] 1. Boot menu on legacy BIOS (normal / safe / recovery / fsck)
- [x] 2. Boot menu on UEFI, with kernel-path fallbacks
- [x] 3. Safe mode: no desktop, no network, text shell
- [x] 4. Recovery mode: read-only filesystem
- [x] 5. Size-independent stage2 via an on-disk boot header
- [x] 6. Installer core: partition, boot chain, ESP, filesystem, account
- [x] 7. `install` shell command (interactive and unattended)
- [x] 8. Graphical installer app
- [x] 9. Graceful `poweroff` / `reboot` (commit + mark clean, then ACPI S5)
- [x] 10. Filesystem consistency checker (`fsck`, automatic after a crash)
        Mount hardening: foreign disks preserved, failed loads protected,
        detached-tree loading and QEMU disk-integrity tests (STORAGE_UPGRADE.md)

## Phase 1 — Reliability & data safety

- [x] 11. Atomic BoltFS commits (A/B slots + mirrored superblock)
- [x] 12. Device cache-flush barriers (ATA / AHCI / NVMe)
- [x] 13. Unclean-shutdown detection and automatic check
- [x] 14. FAT32: overwrite ordering, and startup/recovery no longer formats
        unknown disks. Every FAT and directory read can now fail and be
        reported as a failure instead of as an end of chain or an absent
        file, a directory entry is published by writing the one sector it is
        in rather than read-modify-writing its whole cluster, a new
        directory cluster is written before the FAT links it, and a file
        named by long-name entries can finally be overwritten
- [x] 15. Per-process fault isolation for ring-3 crashes
- [x] 16. Persistent crash log surviving reboot (`crashlog`)
- [x] 17. Kernel message ring and `dmesg`
- [x] 18. Scheduler watchdog: report a starved or wedged thread, and a
        `kill` that really terminates a ring-3 process
- [x] 19. Free kernel stacks and address spaces on process exit
- [x] 20. Out-of-memory handling: the watchdog reports a full heap, a
        fragmented one and every allocation that has failed; failed BoltFS
        loads preserve both the live tree and the disk; the heap coalesces in
        both directions, so memory given back is memory available again;
        kmalloc runs with interrupts off, because the timer preempts into it;
        stray and double frees are counted rather than honoured; and the
        remaining unchecked callers (the filesystem root, the compositor's
        second buffer, Paint's canvas) check now

## Phase 2 — Security & real multi-user

- [x] 21. Strong password hashing (salted, iterated) replacing FNV-1a
- [x] 22. Login rate limiting / backoff on repeated failures
- [x] 23. Session lock screen with password re-entry
- [x] 24. Idle auto-lock
- [x] 25. Per-user home directories, created with the account
- [x] 26. File ownership and permission bits on every node
- [x] 27. VFS-wide permission enforcement (open / create / remove / write)
- [x] 28. `chmod`, `chown`, `ls -l`
- [x] 29. `passwd`, `userdel`, `groups`
- [x] 30. `sudo` and an admin group, replacing implicit trust
- [x] 31. Firewall GUI in Settings (rules list, per-app allow/deny)
- [x] 32. Encrypted vault directory: AES-256-GCM over the whole directory,
        the key stretched from its own password with PBKDF2-HMAC-SHA256 and
        held only while it is open, so the lock screen, a logout, switching
        accounts and shutdown all seal it without asking again. Locking
        commits twice more, so the slot holding the previous image is
        overwritten and the plaintext is not on the disk at all. Replaces a
        `vault` command that XOR'd a file against its password

## Phase 3 — Networking for daily use

- [x] 33. virtio-net driver (broad VM and cloud portability)
- [x] 34. Static and dynamic interface configuration (`ifconfig`)
- [x] 35. Network configuration persisted across reboots
- [x] 36. `/etc/hosts` and a DNS cache
- [x] 37. NTP client, so the clock is right without RTC drift
- [x] 38. IPv6 addressing and neighbour discovery: link-local from the MAC as
        a modified EUI-64, solicited-node multicast groups, a neighbour
        cache, ICMPv6 with the mandatory pseudo-header checksum verified
        before anything is believed, `ping6`, and duplicate address
        detection. Needed two fixes underneath it: the e1000 accepted
        broadcast but not multicast, which is every ND frame there is, and
        netif_poll_all() could be re-entered from inside a packet handler
- [x] 39. SLAAC autoconfiguration: router solicitation, the prefix out of the
        advertisement, and a global address formed and checked before it is
        used. No server involved at all -- that's the whole idea
- [x] 39b. TCP and UDP over IPv6. One `netaddr` (include/netaddr.h) holds
        either family, and the two transports carry both: only the
        pseudo-header the checksum covers and the output path the segment
        leaves by differ, so the handshake, the sequence arithmetic and the
        receive ring never ask which it is. The resolver asks for AAAA as
        well as A, preferring whichever family the machine can actually
        reach; `http://[fe80::1]:8080/` works, brackets and all, through
        http, TLS, ssh and the ring-3 sockets (SYS_CONNECT6 / SYS_ACCEPT6 /
        SYS_RESOLVE6, added rather than folded into the v4 calls so ABI 1
        binaries are untouched). A v6 datagram with no checksum, or a wrong
        one, is dropped rather than delivered -- there is no header checksum
        underneath to have caught it. test/t_tcp6.py puts two BoltOS
        machines on one wire with nothing configured on either and fetches a
        file between them; the UDP path, which nothing on the machine sends
        yet, is checked byte for byte in test/host/ip6.c instead of being
        shipped unrun.
        Still v4-only: the resolver's *transport* (a v6-only machine has
        nowhere to send a query until the RDNSS option in a Router
        Advertisement is parsed), the file-share broadcast protocol, and
        firewall rules that name an address -- a rule holds a uint32_t and a
        CIDR mask, so v6 traffic is filtered on direction, protocol and port
        and by the default policy, which beats not filtering it at all
- [x] 40. TCP listen/accept (server sockets)
- [x] 41. Socket syscalls, so ring-3 programs can use the network
- [x] 42. SSH client: the same machinery as the server with the roles
        swapped, a host key checked against known_hosts, and a changed key
        refused rather than asked about. Two BoltOS machines on one wire in
        test/t_sshclient.py -- which is also where it showed that a machine
        idle at a prompt answered nothing at all, because the NIC's interrupt
        line was never opened in the IO-APIC
- [x] 43. SSH server (remote administration and development)
- [~] 44. Wi-Fi: the station, and a radio to run it against. The blocker was
        real -- QEMU emulates no 802.11 device and there is no card in the
        development machine -- but the answer to it was not to write register
        programming that could never be executed. It was to make the radio the
        replaceable part. `include/wifi.h` now has the seam: a
        `struct wifi_radio` with start, tune, transmit and hand-frames-up, and
        nothing else, because scanning and associating and keying are 802.11
        rather than silicon and belong above it. `net/wifi_sta.c` is a real
        station on top: probe, open authentication, association with an RSN
        element, the handshake, CCMP both ways, the group key for broadcast,
        and a packet number that refuses to go backwards.
        `drivers/wifi_sim.c` is a driver behind that seam -- an access point
        inside the kernel bridged to the wired interface, no PHY -- so every
        line above the radio runs on every boot that joins a network. This is
        mac80211_hwsim's trick, and it is what wpa_supplicant's own tests use.
        **Still open:** a driver for real hardware. Nothing above it changes
        when one arrives, which was the point
- [x] 45. WPA2 four-way handshake, in `net/wpa.c` with the primitives it
        needed and TLS never did: SHA-1, HMAC-SHA1, PBKDF2, the AES inverse
        cipher, RFC 3394 key unwrap, CMAC, CCM, then PMK/PTK, the EAPOL-Key
        MIC and CCMP framing. All arithmetic, so all checked:
        `test/host/wpa.c` against FIPS 180-4, RFC 2202, 6070, 3394, 4493,
        3610 and the 802.11i annexes in a second with no QEMU, and
        `test/t_wifi.py` cross-checks the PRF, PMK, PTK and MIC against
        Python's hashlib -- a second implementation of SHA-1 beats a vector
        typed in from memory, which is how two wrong "expected" values got
        caught with the code being right
- [x] 46. Wi-Fi GUI picker: a Network window with the networks in range,
        signal bars, what security each wants, a passphrase field, connect /
        disconnect / forget, and the radio switch. It drives the same code
        `wifi` drives. The tray's network button names the network it is on
        and opens it
- [x] 47. Wi-Fi credential store, persisted: /etc/wifi.conf, one `ssid psk`
        line per remembered network, mode 0600 because a passphrase is a
        credential. `wifi_autoconnect()` at boot rejoins without being asked

Not implemented and visibly so: SAE, so a WPA3-only network is listed and
refused rather than joined on a downgrade; WEP, refused because it is not
security; TKIP; QoS and 802.11n rates; fragmentation; power save. `wifi` says
which of these it is rather than failing quietly.

## Phase 4 — Development platform

- [x] 48. IDE project tree for real multi-file projects (tree, open-file tabs,
        save-all-then-build)
- [x] 49. Build targets in the IDE, run through BoltCC (`build.txt`; the
        `build` command resolves the same target from the shell)
- [x] 50. BoltCC: C preprocessor (`#include`, `#define`, conditionals)
- [x] 51. BoltCC: arrays, and now structs -- fields, `.` and `->`, `&`,
        arrays of structs, typedef struct, and a field that refers to the
        struct's own type, which is what a linked list needs. A struct is a
        reference rather than a value, because there are no types here to
        tell `f(p)` from `f(&p)` apart and copying would break the one
        people write; the manual says so rather than hiding it
- [x] 52. BoltCC: native x86-64 code generation (`cc -N`). A second backend
        over the same bytecode: the machine stack is the operand stack, so
        there is no register allocator and no class of bug where two values
        believe they own rcx. It emits NASM text that `as` assembles, which is
        slower than encoding directly and buys `cc -S` -- when native code does
        the wrong thing you can read what it was asked to do
- [x] 53. BoltCC: emit linkable objects (`cc -c`). Functions and file-scope
        variables become names the linker resolves; a prototype with no
        definition in this file becomes a call into another object; only the
        unit defining main() carries `_start`. Each object keeps its own copy
        of the runtime as local symbols, which is a few hundred bytes and the
        reason two of them link without every helper colliding. Two units with
        main(), and an initialiser in a unit that has no preamble to run it in,
        are refused by name rather than compiled into something wrong
- [x] 54. `cc`, which builds and runs multi-file C, and `build`, which resolves
        a project's target out of its build.txt
- [x] 55. Assembler for `.asm` sources (`as`). NASM syntax, a working subset of
        the instruction set, three passes because a .data label's address
        depends on how long .text turned out to be. Every encoding is checked
        against real NASM in test/t_asm.py -- 94 operand forms, byte for byte,
        which is the only cheap oracle there is for an assembler
- [x] 56. Linker producing ELF64 executables (`ld`). 64, PC32, PLT32, 32 and
        32S; archives scanned to a fixed point rather than once left to right,
        so an archive that needs a later one still links; a .symtab on the
        output, so a crash in the result comes back with names in it
- [x] 57. `make` with dependency rules. Variables, $(NAME), $@ $< $^, pattern
        rules, .PHONY. It compares each prerequisite against what it was when
        the target was last built rather than asking which is newer: mtime here
        is uptime ticks and is persisted, so after a reboot a freshly edited
        source is *older* than its object and every other make would silently
        skip it
- [x] 58. Static library archiver (`ar`). System V format, so GNU ar reads ours
        and we read its. No symbol index: the linker reads each member's own
        symbol table, and a second copy of that is one more thing to keep true
- [x] 59. BoltVCS: init / add / commit / log / status / show
- [x] 60. BoltVCS: diff (LCS, not a line compare) / checkout / branch
- [x] 61. Terminal text editor (`edit`)
- [x] 62. Core dumps written on a ring-3 crash (trap frame + the mapped
        stack, pruned to the newest 8)
- [x] 63. Post-mortem inspector: `core show` -- registers, a backtrace named
        from the program's own .symtab, and the stack
- [x] 64. Breakpoint / single-step debugger (`cc -g`: breakpoints, stepping,
        backtrace, locals by name; runtime errors report line + call stack)
- [x] 65. Package + repository format (`pkg build`, `pkg publish`, an index
        with hashes)
- [x] 66. Package dependency resolution (`requires`, installed first; `remove`
        refuses while depended on)
- [x] 67. Packages fetched over the network (an http:// repository, hash checked)
- [x] 68. Binary packages, not just scripts (explicit sizes, verbatim payloads,
        `kind program` exec'd by `pkg run`)
- [x] 69. Curated catalogue: headers for `cc`, and a starter project `build` runs
- [x] 70. Documented syscall ABI v1, shared SDK headers, external ELF build
        helper and `run` with exit status (SDK.md)
- [x] 71. Man pages for every registered command (`man`, `help COMMAND`,
        `man -k`, coverage checked by `man -c`)
- [x] 72. Shell scripting: variables, conditionals, loops, functions;
        bounded execution and nested redirection (SHELL.md)
- [x] 73. Per-account history, tab completion and caret editing in the text
        shell and GUI Terminal; clean-exit persistence

## Phase 5 — Desktop UX

- [x] 74. Window tiling: halves, quarters and maximise, from a drag to an edge
        or from Win+arrow, which walks between them the way Windows does. One
        snap_rect() serves the placement and the preview drawn under the window
        while you drag, so they cannot disagree. `snap ZONE` is the same thing
        from a prompt, which is also what makes it testable without
        synthesising a Win key
- [x] 75. Four virtual desktops: per-desktop windows, taskbar and Alt+Tab,
        hit tests included, so a hidden window takes no input. Win+1..4,
        Ctrl+Alt+arrows, Win+Shift+arrows, taskbar pips, and `desk`.
- [x] 76. Configurable keyboard shortcuts: a named-action table resolved in
        the keyboard driver before a key means anything, `keys` to rebind,
        persisted in /etc/keys.conf. Replaces four hard-coded special cases.
- [x] 77. Notification system: a kernel-wide store, stacked toasts, a tray
        notification centre with unread counts, and `notify`. The crash path
        and the storage layer post to it.
- [~] 78. Quick settings: volume slider, network status and a calendar flyout
        exist in the tray. Brightness and battery wait on items 91-92.
- [x] 79. Screenshots: a real PNG encoder, the panel read back (so ring-3
        programs and the text console capture too), `screenshot` for headless
        and SSH use, Print Screen from the desktop. Replaces an uncompressed
        BMP of the compositor's backbuffer.
- [ ] 80. Screen recording
- [x] 81. Display scaling: the panel keeps its native mode and the logical
        desktop becomes it divided by 100/125/150/200%, scaled back up to fill
        the screen -- so every app's fixed pixel layout grows without any app
        knowing. The pointer is mapped through the same rectangle, which is the
        half that would have looked perfect in a screenshot and been unusable.
        Auto reads panel width, because no monitor reports a physical size and
        there is no DPI to have. A scale leaving less than 640x480 is declined
        and says so; `display` is the shell front end, for the machine whose
        desktop came up too small to click Settings on
- [x] 82. Typed clipboard: text, images or file paths. Photos and Paint copy
        and paste pictures, File Explorer publishes paths and writes a pasted
        image out as a PNG, and `clip show` names the kind.
- [x] 83. Software centre: Available/Installed tabs over the repository index,
        one-click install and remove through the same pkg code the command
        uses, work deferred one tick so the desktop paints before it blocks.
- [x] 84. Start-menu search: the app grid filters as you type, and from the
        second character the same query searches the filesystem -- home first,
        five results, permission-checked, held as paths rather than nodes the
        menu could outlive. A file result opens through the explorer's own
        association table rather than a second copy of it

## Phase 6 — Peripherals & media

- [x] 85. Intel HD Audio driver alongside AC97: controller reset, CORB/RIRB
        with an immediate-command fallback, a codec graph walk that scores
        the output pins and traces a route back to a converter through
        mixers and selectors, and gain on that route as the volume knob.
        Both drivers now sit behind one interface, and a controller with
        no codec on its link is declined rather than claimed and silent
- [ ] 86. Volume mixer
- [ ] 87. Microphone capture path
- [x] 88. WAV playback: a RIFF/WAVE decoder that is also the converter --
        resampling, mono to stereo and 8-bit unsigned to signed, decoded
        incrementally so a track streams rather than converting in full.
        `play FILE`, and two system sounds generated at build time
- [ ] 89. MP3 decoding
- [x] 90. Media player app: the folder a track came from is the playlist, it
        advances by itself, and the level meter is measured from the PCM on its
        way to the codec. Fed from media_pump() at the top of the compositor
        loop rather than the window's tick -- a millisecond between buffers
        instead of 150, and playback that survives being minimised, another
        desktop, a locked session and a ring-3 program owning the panel.
        `media` is the same transport from a prompt, and opening the player
        that way does not take the keyboard off the shell. One codec, one
        stream: a blocking `play` preempts the players through a hook in
        drivers/audio.c, and Media and Music stop each other
- [ ] 91. ACPI battery status
- [ ] 92. Backlight / brightness control
- [ ] 93. Suspend (S3) and resume
- [ ] 94. USB mass storage, so USB drives appear in File Explorer
- [ ] 95. Additional keyboard layouts

## Phase 7 — Observability, testing, docs

- [x] 96. Rotating system log file: a clean shutdown now actually writes it
        (klog_persist existed; nothing but `dmesg save` ever called it), and
        `logs` reads it back, with `logs save` for the boot that ends badly
- [x] 97. GUI log viewer: this boot's ring, earlier boots and the crash log,
        tagged and scrollable
- [x] 98. QEMU regression suite runnable headless
- [ ] 99. Fuzzers for the network stack and the HTML/CSS/JS parsers
- [x] 100. User guide (GUIDE.md: install, daily use, troubleshooting, and an
        honest list of what the OS cannot do) and developer guide
        (DEVELOPING.md: the on-machine toolchain, projects, debugging,
        packaging, writing a desktop app, and the testing conventions)
