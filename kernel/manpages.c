#include <stdint.h>
#include "commands.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/manpages.c
 *  `man` -- what each command actually takes, and what it actually does.
 *
 *  `help` lists every command with one line each, which is enough to remember
 *  a name and never enough to use one. A command whose arguments you have to
 *  discover by getting them wrong is a command you avoid, and a shell full of
 *  those is a shell nobody explores.
 *
 *  So every command in the table has a page here: a usage line and a few
 *  sentences of what it is for. Not a book -- the useful part of a manual is
 *  the synopsis and the sentence that says which of the four similar commands
 *  is the one you want.
 *
 *  `man -c` reports any command without a page, and test/t_man.py fails if
 *  there is one, which is what keeps this honest as commands are added.
 * ===========================================================================*/

typedef struct { const char *name, *usage, *body; } manpage_t;

static const manpage_t MAN[] = {

/* ------------------------------------------------------- files & folders -- */
{ "ls", "ls [-l] [PATH]",
  "Lists PATH, or the working directory. -l adds each entry's mode string,\n"
  "owner and size -- which is how you see what chmod and chown did." },
{ "tree", "tree [PATH]",
  "The whole subtree under PATH, indented. Useful before an `rm -r` you are\n"
  "not completely sure about." },
{ "cd", "cd [PATH]",
  "Change the working directory. With no argument, goes to /.\n"
  "`..` is the parent, as everywhere." },
{ "pwd", "pwd",
  "Print the working directory as an absolute path." },
{ "mkdir", "mkdir PATH",
  "Create a directory. The parent must exist." },
{ "rm", "rm [-r] PATH",
  "Remove a file. -r removes a directory and everything under it, and does\n"
  "not ask twice -- `trash` is the one that can be undone." },
{ "cp", "cp SRC DST",
  "Copy a file. DST may be a new name or an existing directory." },
{ "mv", "mv SRC DST",
  "Move or rename. Within one filesystem this relinks the node rather than\n"
  "copying it, so it costs nothing however large the file is." },
{ "find", "find [DIR] PATTERN",
  "Search DIR (default working directory) for names containing PATTERN, printing\n"
  "the full path of each." },
{ "touch", "touch PATH",
  "Create an empty file, or update the modification time of an existing one." },
{ "write", "write [-a] FILE TEXT...",
  "Write TEXT into FILE, replacing what was there. -a appends instead, which\n"
  "is how a file is built up a line at a time from the shell. Quote the text\n"
  "if it contains spaces or a semicolon." },
{ "edit", "edit FILE",
  "A full-screen editor: arrows, Home/End and PgUp/PgDn move; ^S saves,\n"
  "^K cuts a line, ^X exits and offers to save. It draws with ANSI escapes,\n"
  "so it works over serial and SSH as well as on the framebuffer." },
{ "trash", "trash FILE",
  "Move a file to the trash instead of deleting it. `recover` brings it back\n"
  "to where it came from." },
{ "recover", "recover [NAME]",
  "List the trash, or restore NAME to its original path." },
{ "chmod", "chmod MODE PATH",
  "Set the permission bits, in the octal everybody knows: 644 is owner-write\n"
  "and world-read, 755 adds execute. Only the owner (or root) may." },
{ "chown", "chown USER PATH...",
  "Change a file's owner. Only root may give a file away." },

/* ------------------------------------------------------- file inspection -- */
{ "cat", "cat FILE...",
  "Print files, in order. For anything binary, use `hex`." },
{ "head", "head [-N] FILE",
  "The first N lines (default 10)." },
{ "tail", "tail [-N] FILE",
  "The last N lines (default 10)." },
{ "hex", "hex FILE",
  "A hex + ASCII dump. The first thing to reach for when a file is not what\n"
  "you expected it to be." },
{ "meta", "meta PATH",
  "Name, absolute path, type, size, modification time and permissions." },
{ "diff", "diff A B",
  "Compare two files line by line and print the lines that differ. For a\n"
  "diff that understands inserted lines, use `vcs diff` inside a repository." },
{ "grep", "grep PATTERN FILE",
  "Print the lines of FILE containing PATTERN, with line numbers. Plain\n"
  "substring matching, not a regular expression." },
{ "checksum", "checksum FILE",
  "The file's FNV-1a 64-bit hash, in hex. Enough to tell whether two copies\n"
  "are the same file; not a signature." },
{ "preview", "preview FILE",
  "A short look at a file: the first lines if it is text, a summary if not." },
{ "count", "count FILE",
  "Lines, words and characters." },

/* --------------------------------------------------------------- system --- */
{ "sysinfo", "sysinfo",
  "One screen of everything: CPU, memory, disks, filesystem, network and\n"
  "uptime. The place to start when something is wrong." },
{ "cpuinfo", "cpuinfo",
  "Vendor, brand string, family/model/stepping and the CPUID feature bits\n"
  "the kernel cares about." },
{ "meminfo", "meminfo",
  "Physical frames used and free, and what the filesystem is holding." },
{ "mem", "mem",
  "The same report as `meminfo`." },
{ "diskinfo", "diskinfo",
    "Every block device with its transport, size and model, and which one the\n"
    "filesystem is persisted to. READ-ONLY means changes stay in RAM, because\n"
    "of recovery mode or a storage failure. Unknown disks need `install`." },
{ "sync", "sync",
  "Commit the filesystem to disk now. Mutations already autosave; this is\n"
  "for when you are about to do something that might not end tidily." },
{ "fsck", "fsck [-r]",
    "Check the persisted image: both superblocks, the generation counters and\n"
    "the checksum, record bounds, parent directories and names. -r rewrites\n"
    "the image from the live tree to repair it. Runs automatically after an\n"
    "unclean shutdown. A failed mount protects the disk: -r and sync are then\n"
    "disabled so that seed files cannot overwrite the original image." },
{ "poweroff", "poweroff",
  "Commit the filesystem, mark the image clean, then power off through ACPI\n"
  "S5. Use this rather than closing the window: an image left dirty costs the\n"
  "next boot a full check." },
{ "reboot", "reboot",
  "The same shutdown, then a restart." },
{ "install", "install [DISK [USER PASS [ESP_MB]]]",
  "Install BoltOS onto another disk: partition it, write both boot chains\n"
  "(legacy MBR and the UEFI ESP), copy the kernel and the filesystem, and\n"
  "create the first account. With no arguments it lists the disks it can\n"
  "install to. With DISK alone it prompts for the account and confirmation.\n"
  "There is a graphical installer on the desktop too." },
{ "dmesg", "dmesg [-N] [save]",
  "The kernel message ring -- the boot log, long after the console scrolled\n"
  "and the desktop took the screen. -N shows the last N lines; `save` writes\n"
  "it to /var/log/kernel.log." },
{ "crashlog", "crashlog [clear]",
  "Crash reports that survived a reboot: registers, the fault decode and a\n"
  "backtrace for every ring-3 fault and kernel panic." },
{ "core", "core [list | show FILE | rm FILE|all]",
  "Core dumps from ring-3 crashes. `show` decodes the fault, prints the\n"
  "registers, and walks the saved stack into a backtrace with every frame\n"
  "named out of the program's own symbol table. The newest eight are kept." },
{ "uptime", "uptime",
  "How long since boot, and the raw tick count." },
{ "run", "run PATH [args...]",
  "Start an ELF64 program in ring 3, pass the arguments through, wait for it\n"
  "and report its exit status. A name with no '/' is looked for in /bin.\n"
  "See SDK.md for how to build one." },
{ "winrun", "winrun [FILE.exe]",
  "Run a Windows x86-64 console executable through the PE32+ loader.\n"
  "With no file, runs the embedded demo. Arguments are not passed through." },
{ "compile", "compile FILE [OUT.exe]",
  "Pack a .c/.cpp/.cs/.py source into a genuine Windows PE32+ executable with\n"
  "the BoltCC or BoltPython runtime baked in. The result runs on Windows and\n"
  "under `winrun` here." },
{ "battery", "battery",
  "Reports that battery telemetry is unavailable; no battery driver is implemented." },
{ "sensors", "sensors",
  "Shows timer ticks and the RTC. Thermal, voltage and fan drivers are unavailable." },
{ "devices", "devices",
  "The platform devices and every PCI function found, with its class." },
{ "version", "version",
  "OS version, architecture, CPU and the current time." },
{ "health", "health",
  "A quick verdict on memory, disk, filesystem and network, with what to do\n"
  "about anything that looks wrong." },
{ "whoami", "whoami",
  "The current username." },
{ "id", "id",
  "uid, gid, home directory and whether the session is currently elevated." },
{ "groups", "groups [USER]",
  "The groups a user belongs to. Membership of `admin` is what `sudo` checks." },
{ "users", "users [list|add|del|passwd|su] ...",
  "The account table. Sub-commands mirror the standalone commands below." },
{ "useradd", "useradd NAME PASSWORD",
  "Create an account and its home directory. Admin only." },
{ "userdel", "userdel NAME",
  "Remove an account. Admin only; the last admin cannot be removed." },
{ "passwd", "passwd [USER]",
  "Change a password. Your own without an argument; anyone else's as admin.\n"
  "Passwords are stored salted and iterated, never in the clear." },
{ "sudo", "sudo COMMAND [args...]",
  "Run one command as root. Members of the `admin` group only, and the\n"
  "elevation lasts exactly that one command." },
{ "logout", "logout",
  "End the session and return to the sign-in screen. Writes the shell\n"
  "history out on the way." },
{ "lock", "lock",
  "Lock the session; the password unlocks it. Ctrl+Alt+L does the same from\n"
  "anywhere in the desktop." },
{ "pkg", "pkg avail|install|remove|list|info|run|update|repo|build|publish ...",
  "The package manager. Packages come from a repository -- a directory or an\n"
  "http:// URL -- are checked against the index's hash, and bring their\n"
  "dependencies with them. `pkg build` and `pkg publish` make new ones here.\n"
  "`pkg` with no argument lists what is installed." },
{ "sh", "sh FILE [args...]",
  "Run a shell script. The language is the one the prompt speaks, because it\n"
  "is the same interpreter: variables (NAME=value, $NAME, ${NAME}), $? for the\n"
  "last status, $1.. and $# for the arguments, if/elif/else/fi, while and for\n"
  "loops with break and continue, functions written NAME() { ... }, and\n"
  "return and exit. A line beginning with # is a comment.\n"
  "\n"
  "    for f in a b c; do\n"
  "        if test -f $f; then echo $f exists; fi\n"
  "    done\n"
  "\n"
  "Keywords like `then` and `do` end a segment, so they need the `;` before\n"
  "them that every shell has always wanted." },
{ "test", "test EXPR   |   [ EXPR ]",
  "Evaluate a condition and succeed or fail, which is what `if` and `while`\n"
  "read. -f FILE, -d DIR and -e PATH ask about the filesystem; -z and -n ask\n"
  "whether a string is empty; = and != compare strings; -eq -ne -lt -le -gt\n"
  "-ge compare numbers. `[` is the same command and wants a closing `]`." },
{ "source", "source FILE [args...]",
  "Run FILE in this shell session, keeping its variables and functions.\n"
  "This is an alias for sh; BoltShell has no subshells." },
{ ".", ". FILE [args...]",
  "Alias for source. See man sh for the supported scripting language." },
{ "[", "[ EXPR ]",
  "The same command as `test`, spelled the way conditions usually look. The\n"
  "closing bracket is a real argument and has to be there." },
{ "set", "set [NAME VALUE]",
  "With no arguments, list the shell variables. With two, set one -- though\n"
  "NAME=VALUE at the prompt does the same thing and needs no command." },
{ "unset", "unset NAME...",
  "Remove shell variables. An unset variable expands to nothing." },
{ "true", "true",
  "Succeed. `while true; do ...; done` is the loop that needs a `break`." },
{ "false", "false",
  "Fail. Useful for the same reason `true` is." },
{ "man", "man COMMAND | man -k WORD | man -c",
  "The page for one command: what it takes, and which of the similar ones it\n"
  "is. -k searches every page for a word, which is how you find a command\n"
  "whose name you do not know. -c lists commands with no page." },
{ "help", "help [COMMAND]",
  "Every command, by category, with a line each. With a name, that command's\n"
  "manual page, just like `man NAME`." },
{ "echo", "echo TEXT...",
  "Print the arguments back, separated by spaces." },
{ "clear", "clear",
  "Clear the screen." },
{ "notify", "notify [-l LEVEL] [-s SOURCE] TEXT | notify list [-a] | read ID | dismiss ID | clear",
  "Post a notification, or read the ones already posted. A notification is a\n"
  "toast bottom-right now and a row in the desktop's notification centre\n"
  "afterwards, so something that finished while you were looking elsewhere is\n"
  "still there when you look back. LEVEL is info, success, warn or error;\n"
  "SOURCE is who is speaking. `list` shows unread, `list -a` everything,\n"
  "`read` marks without deleting, `dismiss` removes one, `clear` removes all." },
{ "screenshot", "screenshot [PATH]",
  "Capture what is on screen to a PNG. With no PATH it writes the next free\n"
  "~/Pictures/screenshot-N.png. Print Screen (F12) does the same thing from the\n"
  "desktop and opens the result in Photos. It reads the panel back rather than\n"
  "the compositor's buffer, so DOOM, the ring-3 browser and the text console\n"
  "capture correctly too." },
{ "desk", "desk [N] | desk move N | desk list",
  "The virtual desktops. With no argument, lists all four, marks the one you\n"
  "are on and names the windows on each. `desk 2` switches; `desk move 2` sends\n"
  "the focused window there and follows it.\n"
  "From the desktop itself: Win+1..4 switches, Ctrl+Alt+Left/Right walks,\n"
  "Win+Shift+Left/Right takes the focused window along, and the pips at the\n"
  "left of the taskbar do the same with the mouse. A window belongs to the\n"
  "desktop it was opened on; pinned taskbar launchers appear on all of them." },
{ "keys", "keys | keys ACTION CHORD | keys reset [ACTION] | keys save",
  "The global keyboard shortcuts, and how to change them. With no argument,\n"
  "lists every action, its chord and what it does, marking the ones you have\n"
  "changed. A chord is modifiers joined by + and then one key: Ctrl, Alt,\n"
  "Shift, Win, then Left/Right/Up/Down, Home/End, PgUp/PgDn, Del, Tab, Esc,\n"
  "Space, Enter, Back, F1..F12 or a single character.\n"
  "A bare character is refused: binding k on its own would take that letter\n"
  "away from every program. F-keys need no modifier. Two actions cannot share\n"
  "a chord -- unbind one with `keys ACTION none` first. Changes are written to\n"
  "/etc/keys.conf and reloaded at boot." },
{ "logs", "logs [-n LINES] | logs -a | logs save | logs clear",
  "The kernel log that outlives the boot it was written in. `dmesg` shows this\n"
  "boot's ring, which is in RAM; this shows /var/log/kernel.log, which is\n"
  "written there on a clean shutdown and trimmed to stay under a size cap.\n"
  "`logs save` writes the current ring now -- the thing to run before\n"
  "rebooting a machine that has just misbehaved, since a boot that ends badly\n"
  "is exactly the one whose log is otherwise lost. `crashlog` is the separate\n"
  "record of faults and panics." },

/* -------------------------------------------------------------- process --- */
{ "ps", "ps",
  "Every thread: its slot, name, state, CPU time and, for ring-3 programs,\n"
  "the pid you would pass to `kill`." },
{ "kill", "kill PID",
  "Terminate a ring-3 process. Its files and the screen are handed back at\n"
  "once; the memory is reclaimed when the scheduler is no longer standing on\n"
  "it. Kernel threads can only be asked, not forced." },
{ "top", "top",
  "A live view of the same list, refreshing until a key is pressed." },
{ "freeze", "freeze PID",
  "Mark a registry task frozen. Advisory only: does not suspend a scheduler thread." },
{ "resume", "resume PID",
  "Mark a registry task running again. Advisory only; see man freeze." },
{ "services", "services",
  "The registered background services and whether each is running." },
{ "service", "service NAME [start|stop|restart|status]",
  "Update or query a service's registry state. Does not start or stop driver threads." },
{ "jobs", "jobs",
  "Reports that commands run synchronously. Background jobs (&) are not supported." },
{ "priority", "priority PID NICE(-20..19)",
  "Change the advisory priority in the task registry. The scheduler remains round-robin." },
{ "monitor", "monitor [SECONDS]",
  "Sample CPU and memory use over time and print what moved." },

/* -------------------------------------------------------------- network --- */
{ "netinfo", "netinfo",
  "The interface, its address, netmask, gateway and DNS servers, plus the\n"
  "counters the driver keeps." },
{ "ifconfig", "ifconfig [dhcp | IP MASK GW [DNS]]",
  "Show or set the interface. A static configuration is written to\n"
  "/etc/network.conf and applied again at the next boot; `dhcp` goes back to\n"
  "leasing an address." },
{ "hosts", "hosts [add NAME IP]",
  "The local name table, consulted before DNS. Survives reboots." },
{ "dns", "dns [NAME | flush]",
  "Resolve a name, show the cache, or empty it. Cached entries expire on the\n"
  "TTL the server gave them." },
{ "ntp", "ntp [sync | on | off | server IP]",
  "Set the clock from a time server. A wrong clock fails certificate\n"
  "validity checks, which looks like HTTPS being broken rather than like the\n"
  "year being wrong." },
{ "serve", "serve [PORT] [DIR]",
  "An HTTP file server over real accepted connections. Serves DIR (default\n"
  "the working directory) until a key is pressed." },
{ "netget", "netget HOST [PATH] [PORT]",
  "Fetch a URL from a ring-3 program over a real socket -- the demonstration\n"
  "that the socket syscalls work outside the kernel." },
{ "sshd", "sshd [PORT]",
  "The SSH-2 server: a real encrypted shell on this machine. Needs `sudo` for\n"
  "port 22. `sshkey` shows the fingerprint to check on first connection." },
{ "sshkey", "sshkey",
  "The host key's SHA-256 fingerprint, stable across reboots." },
{ "ping", "ping HOST",
  "ICMP echo, with the round-trip time." },
{ "trace", "trace HOST",
  "Placeholder: route tracing is not implemented and the command returns failure." },
{ "ports", "ports",
  "Prints the hardware I/O port map. The network-port display is a legacy\n"
  "placeholder; its 'no stack' message does not describe the current network stack." },
{ "download", "download URL [DEST]",
  "Fetch a URL over HTTP or HTTPS into a file. This is one of the ways a\n"
  "program built elsewhere gets onto the machine (see SDK.md)." },
{ "browse", "browse URL|FILE",
  "Fetch a page and render its text to the terminal -- no layout, just the\n"
  "content. The Browser app is the one with a layout engine." },
{ "domq", "domq URL|FILE SELECTOR",
  "Parse a page and print the elements matching a CSS selector." },
{ "domlayout", "domlayout URL|FILE",
  "Parse and lay out a page, printing the box tree with each box's geometry." },
{ "upload", "upload FILE",
  "Placeholder: upload is unavailable. Use share for LAN transfers or serve for HTTP." },
{ "wifi", "wifi [scan|status]",
  "Wi-Fi state. The radio driver is not built yet, so this reports what the\n"
  "hardware layer knows rather than pretending to associate." },
{ "firewall", "firewall [on|off|status|stats|clear|add|del|default] ...",
  "Packet filtering rules, with per-rule pass and block counters. The\n"
  "Security app on the desktop is the same rules with buttons." },
{ "share", "share [recv [NAME] | send IP PATH | PATH]",
  "Transfer files between BoltOS machines over UDP port 7373. Start share recv\n"
  "on the receiver, then share send IP PATH on the sender. share PATH discovers peers." },
{ "scan", "scan",
  "Enumerate PCI devices. Network discovery is not implemented by this command." },
{ "webx", "webx [URL|PATH]",
  "The web browser as a ring-3 program: it takes the framebuffer, parses,\n"
  "lays out and runs JavaScript entirely outside the kernel, and gives the\n"
  "screen back when you press q." },

/* ------------------------------------------------------------- hardware --- */
{ "fat", "fat ls|cat|write|mkdir ...",
  "Read and write a FAT32 volume -- the filesystem every other operating\n"
  "system can also read." },
{ "ext2", "ext2 ls|cat PATH",
  "Read an EXT2 volume." },
{ "power", "power off|reboot",
  "The same as `poweroff` and `reboot`." },
{ "cpus", "cpus",
  "Every logical CPU the firmware reported and which ones came up under SMP." },
{ "acpiinfo", "acpiinfo",
  "The ACPI tables that were found, and what the APIC and HPET are set to." },
{ "play", "play FILE | play [HZ] [MS]",
  "Play sound through whichever codec the machine has: Intel HDA, an AC'97, or\n"
  "the PC speaker if it has neither.\n"
  "With a path, plays a WAV file -- PCM, 8- or 16-bit, any sample rate and\n"
  "channel count. It is resampled to the 48 kHz stereo the codecs want as it\n"
  "streams, so nothing has to be converted in full before it starts. The system\n"
  "sounds live in /usr/share/sounds.\n"
  "With a number, plays a tone (defaults: 440 Hz, 400 ms). A first argument\n"
  "that does not begin with a digit is taken as a path.\n"
  "Either way it reports how long the audio really lasted -- on a PCM codec\n"
  "that is how long the samples took to play, so a figure well short of the\n"
  "one expected means the audio never reached the speakers." },
{ "media", "media [status|list|open FILE|play|pause|stop|next|prev|track N|seek POS|vol N|repeat on|off]",
  "The Media player's transport, from a prompt. There is one player: this and\n"
  "the window are two sets of controls on the same track.\n"
  "`media open FILE` loads a WAV file, makes the other .wav files in its folder\n"
  "the playlist, shows the window and starts playing. Unlike `play`, it returns\n"
  "immediately -- the desktop's main loop feeds the codec half a second at a\n"
  "time, so the track keeps playing while you do something else, and keeps\n"
  "playing when its window is minimised or on another desktop.\n"
  "The window it opens does not take the keyboard: the caret stays in the\n"
  "shell you typed into, so the next thing you type is a command and not a\n"
  "space bar. Raise the window from the taskbar to use the buttons.\n"
  "`seek` takes M:SS, a number of seconds, or +/-N to move relative to where\n"
  "the track is now. `vol` is the system volume, the same one the tray slider\n"
  "and Ctrl+wheel over the taskbar change.\n"
  "`status` reports the position that is audible now, which is behind the\n"
  "decoder by up to the half second it has already handed to the device.\n"
  "Playback needs the desktop running: in safe mode nothing would feed the\n"
  "codec, and the command says so rather than appearing to work." },
{ "dhcp", "dhcp",
  "Request a lease now, rather than waiting for the boot-time attempt." },
{ "clip", "clip show | clip set TEXT... | clip clear",
  "The system clipboard, shared with every GUI app. It carries one item of one\n"
  "kind: text, an image, or a list of files. `show` says which -- an image\n"
  "reports its size, files list their paths.\n"
  "Copy a picture with Ctrl+C in Photos or Paint; paste it into Paint, or into\n"
  "a folder in File Explorer, where it is written out as a PNG. Copying a file\n"
  "in File Explorer also puts its path here as text, so it can be pasted into\n"
  "Notes or a Terminal." },
{ "crash", "crash ring3 [null|div|ud|stack] | crash null|div|ud",
  "Fault on purpose. `crash ring3` runs /bin/crashme, which dies while the\n"
  "machine carries on -- that is the per-process isolation demonstration, and\n"
  "it leaves a core dump for `core show`. Without `ring3` it panics the\n"
  "kernel, which is exactly as drastic as it sounds." },

/* ------------------------------------------------------------ languages --- */
{ "python", "python [FILE.py]",
  "The BoltPython interpreter: a REPL, or run a file." },
{ "cc", "cc [-E] [-g] [-o OUT] [-I DIR] [-D NAME[=VAL]] FILE...",
  "Compile and run C, C++ or C#. Several files are preprocessed and combined\n"
  "into one translation unit -- a unity build, since BoltCC has no linker --\n"
  "so a program really can be split across files with headers between them.\n"
  "-E prints the preprocessed text, -g runs it under the debugger, and the\n"
  "dialect comes from the first file's extension." },
{ "build", "build [-t TARGET] [-l] [-g] [-o FILE] [DIR]",
  "Build a project rather than a file list: DIR's build.txt names its\n"
  "targets, each with its sources, include directories and macros. -l lists\n"
  "them without building. A directory with no build.txt builds every source\n"
  "in it. The Code app's Run button resolves the same target." },
{ "vcs", "vcs init|add|status|commit|log|show|diff|checkout|branch ...",
  "Local version control. `vcs init` starts a repository in the working\n"
  "directory; `add` stages, `commit -m` records, `log` walks the history,\n"
  "`diff` shows what changed since the last commit, and `checkout N` puts an\n"
  "earlier commit back on disk." },
{ "js", "js [-c CODE | FILE.js]",
  "The JavaScript engine, running in ring 3." },
{ "libctest", "libctest",
  "Self-test the freestanding C library: string, memory, printf, math, files." },
{ "nstest", "nstest",
  "Self-test the ported NetSurf libraries (nsurl, libwapcaplet, libhubbub,\n"
  "libcss, libdom)." },

/* ----------------------------------------------------------------- bonus -- */
{ "focus", "focus [on|off]",
  "Toggle the focus flag and record the event. No notification service is connected yet." },
{ "snapshot", "snapshot [save NAME | restore NAME | list]",
  "Copy the current directory tree into /.snap/NAME. Restore merges it into\n"
  "the current directory; extra files are not removed. vcs keeps commit history." },
{ "timeline", "timeline",
  "What has happened on this machine, in order." },
{ "vault", "vault lock|unlock FILE PASSWORD",
  "A toy encrypted store. Named honestly: it is not a security boundary." },
{ "doctor", "doctor",
  "Look for the things that commonly go wrong and offer to fix them." },
{ "assistant", "assistant [QUESTION]",
  "The built-in terminal assistant: what a command is for, and which one to\n"
  "use for a given job." },
{ "sandbox", "sandbox COMMAND [args...]",
  "Run a command with /tmp/.sandbox as its working directory, then restore cwd.\n"
  "Absolute paths still work; this does not isolate filesystem writes or privileges." },
{ "workspace", "workspace [list | save NAME | restore NAME]",
  "Save a working-directory bookmark in memory, or change to a saved directory.\n"
  "Bookmarks last for this boot; window layouts are not saved." },
{ "panic", "panic",
  "Ask for confirmation, then trigger a kernel panic. Recovery requires a reboot." },
{ "story", "story",
  "The machine's log, written as prose rather than as records." },
};

#define NMAN ((int)(sizeof(MAN) / sizeof(MAN[0])))

static const manpage_t *find(const char *name) {
    for (int i = 0; i < NMAN; i++)
        if (!strcmp(MAN[i].name, name)) return &MAN[i];
    return 0;
}

static int contains(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 0;
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* One page: the command's own summary from the table, then usage and body. */
static void show(const manpage_t *m) {
    const command_t *c = cmd_lookup(m->name);
    kprintf("%s - %s\n\n", m->name, c ? c->help : "");
    kprintf("usage: %s\n\n", m->usage);
    kprintf("%s\n", m->body);
}

int cmd_man(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: man COMMAND     the page for one command\n"
                "       man -k WORD     every page mentioning WORD\n"
                "       man -c          commands with no page (should be none)\n"
                "\n%d pages. `help` lists every command with a line each.\n", NMAN);
        return 0;
    }

    if (!strcmp(argv[1], "-c")) {
        int missing = 0;
        for (int i = 0; i < commands_count; i++)
            if (!find(commands[i].name)) { kprintf("  no page: %s\n", commands[i].name); missing++; }
        kprintf("%d command%s, %d page%s, %d missing\n",
                commands_count, commands_count == 1 ? "" : "s",
                NMAN, NMAN == 1 ? "" : "s", missing);
        return missing ? 1 : 0;
    }

    if (!strcmp(argv[1], "-k")) {
        if (argc < 3) { kprintf("usage: man -k WORD\n"); return 1; }
        int hits = 0;
        for (int i = 0; i < NMAN; i++) {
            if (!contains(MAN[i].name, argv[2]) && !contains(MAN[i].usage, argv[2]) &&
                !contains(MAN[i].body, argv[2])) continue;
            const command_t *c = cmd_lookup(MAN[i].name);
            kprintf("  %-10s %s\n", MAN[i].name, c ? c->help : MAN[i].usage);
            hits++;
        }
        if (!hits) kprintf("  (nothing mentions '%s')\n", argv[2]);
        return 0;
    }

    const manpage_t *m = find(argv[1]);
    if (!m) {
        if (cmd_lookup(argv[1]))
            kprintf("man: no page for '%s' yet -- `help %s` has its summary\n",
                    argv[1], argv[1]);
        else
            kprintf("man: no such command: %s\n", argv[1]);
        return 1;
    }
    show(m);
    return 0;
}
