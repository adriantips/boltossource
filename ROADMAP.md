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

Status: `[x]` shipped · `[~]` in progress · `[ ]` not started

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

## Phase 1 — Reliability & data safety

- [x] 11. Atomic BoltFS commits (A/B slots + mirrored superblock)
- [x] 12. Device cache-flush barriers (ATA / AHCI / NVMe)
- [x] 13. Unclean-shutdown detection and automatic check
- [x] 14. FAT32 crash-safe write ordering
- [x] 15. Per-process fault isolation for ring-3 crashes
- [x] 16. Persistent crash log surviving reboot (`crashlog`)
- [x] 17. Kernel message ring and `dmesg`
- [x] 18. Scheduler watchdog: report a starved or wedged thread, and a
        `kill` that really terminates a ring-3 process
- [x] 19. Free kernel stacks and address spaces on process exit
- [~] 20. Out-of-memory handling (watchdog warns at 90% heap; allocation
        failures are still silent in most callers)

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
- [ ] 32. Encrypted home directory / vault for the persisted image

## Phase 3 — Networking for daily use

- [x] 33. virtio-net driver (broad VM and cloud portability)
- [x] 34. Static and dynamic interface configuration (`ifconfig`)
- [x] 35. Network configuration persisted across reboots
- [x] 36. `/etc/hosts` and a DNS cache
- [x] 37. NTP client, so the clock is right without RTC drift
- [ ] 38. IPv6 addressing and neighbour discovery
- [ ] 39. SLAAC autoconfiguration
- [x] 40. TCP listen/accept (server sockets)
- [x] 41. Socket syscalls, so ring-3 programs can use the network
- [ ] 42. SSH client
- [x] 43. SSH server (remote administration and development)
- [ ] 44. Wi-Fi radio driver
- [ ] 45. WPA2 four-way handshake
- [ ] 46. Wi-Fi GUI picker (SSID list, passphrase entry) wired to DHCP
- [ ] 47. Wi-Fi credential store, persisted

## Phase 4 — Development platform

- [ ] 48. IDE project tree for real multi-file projects
- [ ] 49. Build targets in the IDE, run through BoltCC
- [x] 50. BoltCC: C preprocessor (`#include`, `#define`, conditionals)
- [~] 51. BoltCC: arrays (structs and pointers still to come)
- [ ] 52. BoltCC: native x86-64 code generation
- [ ] 53. BoltCC: emit linkable objects
- [~] 54. `cc` command (builds + runs multi-file C; bytecode, not yet an ELF)
- [ ] 55. Assembler for `.asm` sources
- [ ] 56. Linker producing ELF64 executables
- [ ] 57. `make` with dependency rules
- [ ] 58. Static library archiver
- [ ] 59. BoltVCS: init / add / commit / log
- [ ] 60. BoltVCS: diff / checkout / branch
- [x] 61. Terminal text editor (`edit`)
- [ ] 62. Core dumps written on a ring-3 crash
- [ ] 63. Post-mortem inspector: registers, stack, ELF symbols
- [x] 64. Breakpoint / single-step debugger (`cc -g`: breakpoints, stepping,
        backtrace, locals by name; runtime errors report line + call stack)
- [ ] 65. Package repository format
- [ ] 66. Package dependency resolution
- [ ] 67. Packages fetched over the network
- [ ] 68. Binary packages, not just scripts
- [ ] 69. Curated ports catalogue of dev tools
- [ ] 70. Documented, stable syscall ABI and SDK headers
- [ ] 71. Man pages for every shell command
- [ ] 72. Shell scripting: variables, conditionals, loops, functions
- [ ] 73. Shell history and tab completion

## Phase 5 — Desktop UX

- [ ] 74. Window snapping and tiling
- [ ] 75. Virtual desktops / workspaces
- [ ] 76. Configurable keyboard shortcuts
- [ ] 77. Notification system: toasts and a notification centre
- [ ] 78. Quick-settings panel (volume, brightness, network, battery)
- [ ] 79. Screenshot tool built into the OS
- [ ] 80. Screen recording
- [ ] 81. HiDPI / display scaling
- [ ] 82. Clipboard for images and files, not just text
- [ ] 83. Software centre GUI over the package manager
- [ ] 84. Start-menu search across apps and files

## Phase 6 — Peripherals & media

- [ ] 85. Intel HD Audio driver alongside AC97
- [ ] 86. Volume mixer
- [ ] 87. Microphone capture path
- [ ] 88. WAV playback
- [ ] 89. MP3 decoding
- [ ] 90. Media player app
- [ ] 91. ACPI battery status
- [ ] 92. Backlight / brightness control
- [ ] 93. Suspend (S3) and resume
- [ ] 94. USB mass storage, so USB drives appear in File Explorer
- [ ] 95. Additional keyboard layouts

## Phase 7 — Observability, testing, docs

- [ ] 96. Rotating system log file and a `logs` command
- [ ] 97. GUI log viewer
- [x] 98. QEMU regression suite runnable headless
- [ ] 99. Fuzzers for the network stack and the HTML/CSS/JS parsers
- [ ] 100. User guide and developer guide (install, daily use, writing apps)
