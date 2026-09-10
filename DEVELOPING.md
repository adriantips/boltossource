# Developing on BoltOS

Writing software *on* BoltOS rather than for it: what the tools are, how to use
them, and how to add to the operating system itself.

Three separate things live under that heading, and they have different rules:

1. **Programs you write on the machine**, in the Code app or a terminal, built
   and run by BoltCC. No host toolchain, no cross-compiler.
2. **Native ELF64 programs**, built against the published ABI. Today these are
   built on a host and brought over; the ABI is [SDK.md](SDK.md).
3. **The OS itself** — a kernel change, or a new desktop app. Built on a
   Windows/msys2 host with clang and nasm.

- [Programs on the machine](#programs-on-the-machine)
- [Projects and targets](#projects-and-targets)
- [Debugging](#debugging)
- [Version control](#version-control)
- [Packaging and installing](#packaging-and-installing)
- [Native ELF programs](#native-elf-programs)
- [Working on BoltOS itself](#working-on-boltos-itself)
- [Writing a desktop app](#writing-a-desktop-app)
- [Testing](#testing)
- [What is not there yet](#what-is-not-there-yet)

---

## Programs on the machine

BoltOS carries its own compilers: **BoltCC** for C, C++ and C#, and **BoltPy**
for Python. They are interpreters over a bytecode VM, not native code
generators — see [What is not there yet](#what-is-not-there-yet) for what that
costs you.

The shortest possible loop:

```sh
edit hello.c            # a full-screen editor: arrows, ^S saves, ^X exits
cc hello.c              # compile and run
```

`cc` takes as many files as you give it:

```sh
cc main.c util.c parse.c
```

Other languages:

```sh
cc app.cpp              # C++
cc app.cs               # C#
python script.py        # BoltPy, or `python` on its own for a REPL
js script.js            # BoltJS
```

### The preprocessor

`#include`, `#define`, `#ifdef`/`#if`/`#else`/`#endif`, `#undef` and macros with
arguments all work, and `-D` and `-I` do what you expect:

```sh
cc -DDEBUG=1 -Iinc main.c util.c
```

`#include "greet.h"` is resolved relative to the including file, then against
each `-I`. `#include <stdio.h>` finds the headers the `boltlib` package installs.

---

## Projects and targets

A file list on a command line lives in the head of whoever types it. Writing it
down next to the sources is what makes a directory of files a project.

A project is a directory with a **`build.txt`**:

```
# two programs sharing one utility file
target hello
  src main.c greet.c
  define DEBUG=1          # a -D macro, for this target only
  include inc             # a -I directory, relative to the project

target tool
  src tool.c greet.c
```

Then:

```sh
build                     # build and run the first target here
build /home/Documents/hello
build -t tool /proj       # a different target of the same project
build -l /proj            # list the targets and their sources, build nothing
build -g                  # run it under the debugger
build -o all.c            # write the combined translation unit instead of running
```

Lines before the first `target` form a target named after the directory, so a
one-target project is just `src main.c greet.c`. A directory with **no**
`build.txt` is still a project: its target is every source file in it. The
common case should not need a file to describe it.

The **Code** app opens the same projects: a tree on the left, tabs across the
top, and a Run button that saves everything and builds the target through
exactly the same code path the `build` command uses. A project that built on the
desktop and failed over SSH would be a bug nobody could find.

`build.txt` is deliberately a list and not a language — no rules, no variables,
no recipes. BoltCC compiles a whole program in one pass, so there is nothing for
a dependency graph to skip. `make` belongs with separate compilation, and will
arrive with it.

---

## Debugging

### When it goes wrong on its own

A runtime error names its line and prints the call stack that reached it:

```
BoltCC: line 1: runtime: array index out of range
  call stack:
    at()  called from line 6
```

### When you want to stop and look

`cc -g` (or `build -g`) runs the program under a source-level debugger. It stops
before the first line and takes commands:

```
b [line]    set a breakpoint (default: the current line)
d [line]    delete one breakpoint, or all of them
s / n       step to the next line
c           continue
bt          backtrace: every frame and the line it is on
p NAME      print a local, or a global, by name
locals      print every named local in this frame
l [line]    list source around a line
q           stop the program
```

`p` and `locals` work by name because each function keeps its slot-name table
past codegen. `l` lists the *preprocessed* unit, which is what the line numbers
refer to, so a program built from several files lists the text the debugger is
actually stepping through.

### When a native program crashes

Ring-3 crashes are contained: the program dies, the desktop and everything else
keep running, a notification tells you what happened, and a core dump is
written.

```sh
core list        # the dumps, newest first
core show        # the newest: registers, a backtrace, the stack
core show 3      # a specific one
```

The backtrace is named from the program's own `.symtab`, so frames have function
names rather than addresses.

### When the machine itself goes wrong

```sh
dmesg            # the kernel log for this boot
crashlog         # kernel crashes, kept across reboots
notify list -a   # everything the machine has reported
```

---

## Version control

**BoltVCS** is a local version control system, on the machine, in `.bvcs`:

```sh
vcs init                  # start tracking this directory
vcs add main.c            # stage a file
vcs commit "what I did"   # record it
vcs status                # what has changed since
vcs diff                  # the actual lines (an LCS diff, not a line compare)
vcs log                   # the history
vcs show HASH             # one commit
vcs checkout HASH         # put an earlier commit back on disk
vcs branch NAME           # start a branch here
```

It is not git and does not speak git's protocol. It is a history of a project
that lives on the machine that has the project.

---

## Packaging and installing

`pkg` is a real package manager over a documented format: a text header, a blank
line, then each file with an explicit size and a verbatim payload — which is
what lets one carry an ELF as easily as a script.

```sh
pkg avail [TERM]        what the repository offers
pkg install NAME        dependencies first, hash checked, files recorded
pkg list                what is installed
pkg info NAME           one package, and every file it owns
pkg remove NAME         take back exactly what it put down
pkg run NAME            run an installed script or program
pkg repo [URL|PATH]     show, or change, the repository in use
```

The repository is a path *or* an `http://` URL, and nothing downstream of the
fetch knows which — so installing from a disk and installing over the network
are the same code path, and both check the bytes against the index's hash before
writing anything.

### Making one

```sh
pkg build /home/Documents/myproj          # -> myproj-1.0.pkg
pkg publish myproj-1.0.pkg /srv/repo      # add it to a repository and reindex
```

`pkg build` reads a `pkg.txt` in the directory for the name, version, summary,
kind and `requires` list. A program written on BoltOS can be packaged on BoltOS
and installed on the next machine, with no host toolchain in that sentence.

---

## Native ELF programs

A native program is a **static ELF64 executable for x86-64**, loaded at
`0x400000`, entered at `_start`, talking to the kernel through the syscall ABI.
[SDK.md](SDK.md) is the full specification: the memory layout, the syscall
convention and every call, the C runtime, and a worked example.

The short version:

```sh
# on a host with clang:
sdk/build.sh -o myapp main.c util.c

# on BoltOS, once the file is there:
chmod 755 /bin/myapp
run myapp arg1 arg2
```

`run` reports the exit status and the shell exposes it as `$?`. Malformed ELF
headers, out-of-bounds segments and kernel-address destinations are rejected
before anything is mapped.

A program can check what it is running on:

```c
#include "ulibc.h"
if (bolt_abi() != BOLT_ABI_VERSION) { /* built for a different kernel */ }
```

The ABI version is bumped when a syscall's number, arguments or meaning change —
never when one is added, because a program that does not call a new syscall
cannot be broken by it.

**This still needs a host toolchain.** BoltCC does not emit native code yet, so
a native program cannot be built on the machine it runs on. That is the largest
remaining gap in the story, and it is [tracked as roadmap items
52–58](ROADMAP.md).

---

## Working on BoltOS itself

### The toolchain

The kernel is built on Windows with msys2:

```
pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld nasm
```

plus a host Python (with `pycdlib` if you want the bootable ISO). `build.sh`
finds all of it — set `BOLTOS_PYTHON` if it picks the wrong interpreter.

### Building and running

```sh
bash build.sh             # boot chain, kernel, disk image, ISO, ESP
bash run.sh               # boot it in QEMU, serial on stdio
bash run-uefi.sh          # the same, on OVMF firmware
```

`build.sh` produces `iso/os.img` (a raw disk), `iso/boltos.iso` (bootable CD) and
`esp/` (an EFI system partition). It fails the build if stage2 outgrows its 32
sectors, which is the kind of thing you find out about at boot otherwise.

### The layout

```
boot/       stage1, stage2, the UEFI loader
kernel/     the kernel: scheduler, VM, VFS, GUI, apps, compilers, commands
drivers/    hardware: framebuffer, keyboard, mouse, ATA/AHCI/NVMe, e1000, xHCI…
fs/         filesystems: BoltFS (ramfs.c), EXT2, FAT32
net/        the IP stack: ethernet, ARP, IP, TCP, UDP, DHCP, DNS, TLS, HTTP, SSH
mm/         physical and virtual memory
libc/       the freestanding C library the kernel is built against
user/        ring-3 programs and the userland C runtime
include/    every header, including the public ABI
test/       the QEMU regression suite
```

### Adding a shell command

Four steps, and the fourth is enforced:

1. Write `int cmd_thing(int argc, char **argv)` in a `kernel/cmd_*.c`.
2. Declare it in `include/commands.h`.
3. Add a row to the table in `kernel/shell.c` — name, category, one-line help,
   function.
4. Add a page to `kernel/manpages.c`. **`man -c` fails if a command has no
   page**, and `test/t_man.py` fails the build if `man -c` does, so this is part
   of adding a command rather than something to do later.

Then add the file to `SRCS` in `build.sh`.

---

## Writing a desktop app

Every app is one `kernel/app_*.c` file that registers a window and a few
callbacks. The compositor does the framing, dragging, focus, z-order,
minimise/maximise, the taskbar button and the title bar; the app draws its
client area and answers input.

```c
#include "gui.h"

typedef struct { int count; } counter_t;
static counter_t st;
static window_t *win;

static void counter_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)cw; (void)ch;
    char n[16];
    snprintf(n, sizeof n, "%d", st.count);
    g_text(cx + 16, cy + 16, "Clicks so far:", COL_TEXT_DIM, 1);
    g_text(cx + 16, cy + 40, n, COL_TEXT, 3);
}

static void counter_click(window_t *w, int lx, int ly) {
    (void)w; (void)lx; (void)ly;
    st.count++;
    gui_request_redraw();
}

void counter_app_init(void) {
    win = gui_add_window("Counter", 320, 200, 0x59B6FF, ICON_CALC);
    win->draw  = counter_draw;
    win->click = counter_click;
    win->st    = &st;
}
```

Then declare `counter_app_init` in `include/gui.h`, call it from the app-init
block in `gui_run()` (`kernel/gui.c`), and add the file to `SRCS` in `build.sh`.

### The callbacks

| | |
| --- | --- |
| `draw(w, cx, cy, cw, ch)` | Paint the client area. Clipped for you. |
| `key(w, c)` | A character, when this window has focus. |
| `click(w, lx, ly)` | Left click, in client-local coordinates. |
| `rclick(w, lx, ly)` | Right click. |
| `drag(w, lx, ly)` | Motion with the button held. |
| `scroll(w, delta)` | Wheel, `+` up and `-` down. |
| `tick(w)` | About twice a second, whether or not anything happened. |

### Drawing

Everything paints into the compositor's backbuffer and is flipped once a frame,
so there is no flicker and no partial redraw to manage:

```c
g_fill(x, y, w, h, colour);          g_rect(x, y, w, h, colour);
g_round(x, y, w, h, r, colour, a);   g_blend(x, y, w, h, colour, a);
g_text(x, y, "monospace", colour, scale);
g_text_pn(x, y, s, len, colour, scale, italic);   /* proportional */
g_blit(dx, dy, dw, dh, argb, sw, sh);
gui_icon(ICON_FILES, x, y, scale, colour);
```

Colours come from the live theme — `COL_TEXT`, `COL_PANEL`, `COL_ACCENT`,
`COL_GOOD`, `COL_WARN`, `COL_BAD` — so the Settings app can retint the whole
desktop and your app follows. They read the current palette on each use, so
never put a `COL_*` in a static initialiser.

### Talking to the rest of the system

```c
gui_toast("Saved");                            /* a passing notice */
gui_notify(NOTE_ERROR, "Backup", "disk full"); /* and one that is kept */
gui_prompt("Save as", "untitled.txt", on_ok);  /* a modal text prompt */
clip_set(text, len);  clip_get();              /* the system clipboard */
fs_lookup(path); fs_create(path, 0); fs_write(node, data, len);
```

Anything you post with `gui_notify` is kept in the notification centre and
mirrored to the kernel log, so it is visible over a serial console too.

---

## Testing

The suite boots real BoltOS guests in QEMU and asserts on the serial log:

```sh
python test/run_all.py              # everything
python test/run_all.py shell net    # only tests whose name matches
python test/t_shot.py --host-only   # the parts that need no VM
```

Each test is a standalone `test/t_*.py`. `test/boltvm.py` is the harness: it
boots a guest, types at it through QMP, waits for output, takes screenshots and
can pull the power out from under it.

Two habits worth copying from the existing tests:

- **Wait for a prompt, never for a fixed time.** `vm.run()` blocks until the
  next prompt. Every `sleep` is a guess that stops being true the moment the
  host is busy, and a test that fails only under load is worse than no test.
- **Do not assert on something the echo could satisfy.** The serial log carries
  everything typed at it, so waiting for a marker just after typing a command
  containing that marker is an assertion that cannot fail.

Code that can run on the host should be tested there too — it is faster and the
failures are legible. `test/host/storage.c` compiles the real block layer and
filesystem against a failing allocator; `test/host/png.c` compiles the PNG
encoder and the test decodes its output with Python's own zlib.

---

## What is not there yet

The honest list, because it determines what you can actually do:

- **BoltCC does not emit machine code.** It compiles to bytecode and interprets
  it. Programs written on BoltOS run on BoltOS, under `cc`, and cannot be turned
  into a standalone ELF. Native codegen, an assembler and a linker are roadmap
  items 52–58 and they are the difference between this being a development
  environment and a development *platform*.
- **BoltOS cannot build BoltOS.** Following from the above, kernel work needs
  the host toolchain.
- **No `make`, no static libraries, no separate compilation.**
- **BoltCC's C is a large subset, not C99.** Structs and pointers are still
  landing (roadmap item 51).
- **There is no SSH client**, only a server — you can develop on a BoltOS
  machine remotely, not from one.

[ROADMAP.md](ROADMAP.md) tracks all of it, with what is done and what is not.
