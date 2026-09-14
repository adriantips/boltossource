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
for Python. BoltCC has two backends — a bytecode VM, which is the whole
language, and a native x86-64 one, which is an integer subset and produces
programs the machine executes directly. `as`, `ld`, `ar` and `make` are here
too, so a program can be split across files, compiled a file at a time, kept in
a library and rebuilt by rules. See [What is not there yet](#what-is-not-there-yet)
for the edges.

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

And when you want a program rather than a run:

```sh
cc -N hello.c           # compile to a native ELF64, then: run ./hello
cc -S hello.c           # print the x86-64 it would emit
```

`cc -N` covers integers, functions, control flow, `printf` and `puts`. Anything
outside that — arrays, structs, the builtin library, strings used as values — is
refused **by name**, so you find out what to take out rather than that it
cannot be done.

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

You do not need a host for this any more. `sdk/build.sh` is still the way to
cross-compile from a machine with clang, but everything below builds the same
kind of file on BoltOS itself.

---

## One file at a time

`cc a.c b.c` preprocesses both files and compiles them as one translation unit.
That works, and it means rebuilding one file rebuilds everything, and nothing
can be linked against hand-written assembly or against a library.

Separate compilation is the other way:

```sh
cc -c main.c -o main.o      # a relocatable object, not a program
cc -c lib.c  -o lib.o
ld -o prog main.o lib.o
run prog
```

A function this file calls but does not define is declared the way C has always
declared one, and the linker finds it:

```c
/* main.c */
int answer(int n);          /* lives in lib.c */
int main(void) { return answer(20); }
```

Only the unit that defines `main()` carries the entry point, so exactly one of
your objects has one — the same arrangement as `crt0.o` everywhere else.

Assembly works the same way, and mixes freely with C objects:

```sh
as -c fast.asm -o fast.o
ld -o prog main.o fast.o
```

In the assembly, `global` publishes a name and `extern` borrows one.

### Libraries

```sh
ar r libmine.a a.o b.o      # add or replace members
ar t libmine.a              # list
ld -o prog main.o libmine.a
```

A member joins the link only if it defines a name still missing, and the
archives are rescanned until a pass adds nothing — so an archive that needs a
later archive links whichever way round you list them, which is not true of
`ld(1)`.

### Rules

```make
OBJS = main.o lib.o

prog: $(OBJS)
    ld -o $@ $^

%.o: %.c
    cc -c $< -o $@

.PHONY: clean
clean:
    rm prog $(OBJS)
```

`make` builds the first target; `make clean` builds that one. `-n` prints the
recipes without running them.

Two things to know. A recipe may be indented with **spaces** — a tab cannot be
typed into a file from this shell at all, since Tab asks for filename
completion. And a target is rebuilt when a prerequisite *differs* from what it
was when that target was last built, not when it is newer: file times here are
uptime ticks and they are persisted, so after a reboot a source you just edited
is numerically *older* than the object built from it last week, and a
timestamp-comparing make would silently skip it. The record lives in
`.make.stamp` beside the makefile; delete it and the next build does everything
once.

### What does not work yet

Sharing a *variable* between units. BoltCC skips `extern` as a modifier, so
`extern int counter;` reads as a definition and the second unit is a duplicate
symbol. Functions are fine; variables have to live in one unit and be reached
through a function for now. The linker says so rather than picking one.

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

Then add the file to `SRCS` in `build.sh` — there is no globbing, and a source
that is not in that list is a source that silently does not exist.

One trap that only shows up at link time: `build.sh` has a **second** link line
further up, the BoltRT stub that bakes BoltCC into a Windows `.exe`. Anything
`kernel/boltcc.c` or `kernel/ccnative.c` calls into has to be on that line too,
and that link sees only `libc/string.c` — which has `kstrlcat` but *not*
`strlcpy`/`strlcat`. A single-file compile check will not catch either.

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

Code that can run on the host should be tested there too — it is faster, the
failures are legible, and some cases cannot be staged in a VM at all.
`test/host/storage.c` compiles the real block layer and filesystem against a
failing allocator; `test/host/png.c` compiles the PNG encoder and the test
decodes its output with Python's own zlib.

The strongest version of this is an **external oracle** — something that
answers the same question and was not written here. `test/host/asm.c` hands
every instruction to BoltAS and to NASM and compares the bytes.
`test/host/link.c` gives our objects to GNU `readelf` and to LLVM's `ld.lld`,
and gives NASM's objects to our linker. `test/host/wpa.c` checks the WPA key
schedule against Python's `hashlib`, which caught two expected values that had
been typed in wrong while the C was right.

`test/host/make.c` is the other shape: the real `kernel/make.c` over a
filesystem of 64 files in an array and a clock that is a variable, because the
case that matters — an mtime that goes backwards across a reboot — cannot be
staged in QEMU without rebooting.

### Fuzzing

`test/t_fuzz.py` is the third shape: the shipping parsers, a substituted
allocator, and inputs nobody would write on purpose. It covers the network
stack and the browser's HTML/CSS/JS parsers — the two places where the bytes
are chosen by someone else — and it boots no machine, because a fuzzer wants
millions of inputs and QEMU gives you one boot a minute.

```sh
python test/t_fuzz.py                   # 23 targets, about a minute
python test/t_fuzz.py --iters 200000    # a soak
python test/t_fuzz.py --seed 7 web      # one harness, a different sequence
```

To add a target, write a function that takes bytes and hand `fz_main` a
`fz_target` naming it, with a seed corpus of things that already parse. Two
rules, both learned the hard way:

- **Add the check to `fz_selftest()` first.** A harness that is not reaching
  the code it names reports a clean run at enormous speed, which is
  indistinguishable from a clean run that found nothing. The network harness's
  DNS stub had its return convention backwards and every HTTP target exited
  before parsing a byte.
- **Do not keep state between iterations.** The guarded heap is wiped between
  inputs, so anything held across them is read after free by the fuzzer itself
  — and a fuzzer that reports its own bug is worse than none.

When it finds something, the report names the file it saved the input to.
Rename that file for the bug and put it in `test/corpus/<harness>/<target>/`;
every run replays it unmutated before fuzzing anything. A name ending
`.oomN.bin` is replayed with the heap set to start refusing after N
allocations, which several of the saved cases need.

---

## What is not there yet

The honest list, because it determines what you can actually do:

- **BoltOS cannot build BoltOS.** The toolchain is real now — compiler,
  assembler, linker, archiver, make — but BoltCC's C is not the C the kernel is
  written in, and its native backend takes an integer subset. Kernel work still
  needs the host toolchain. This is the last big one.
- **`cc -N` and `cc -c` take a subset.** Integers, functions, control flow,
  `printf` and `puts`. Arrays, structs, the builtin library and strings used as
  values are refused by name; the VM (`cc` with no flags) runs all of them.
- **A file-scope variable cannot be shared between objects.** `extern` on a
  variable is indistinguishable from a definition here, so the second unit is a
  duplicate symbol. Functions cross object boundaries fine.
- **BoltCC's C is a large subset, not C99.** Structs landed; pointers, unions,
  enums, function pointers and floating point have not.
- **`make` has no conditionals, no `include`, no `$(wildcard)`, no parallel
  jobs.** Each is absent because nothing has wanted it yet.
- **A script gets about thirty frames of recursion, not thousands.** BoltJS is
  a tree-walker: it recurses into C once per node evaluated, and the thread it
  runs on has a 64 KB kernel stack (`kernel/sched.c` `KSTACK`). Half of that is
  the budget, checked by measuring the stack rather than counting frames, and a
  script that goes past it gets "too much recursion" instead of taking the
  compositor down with it. Deep enough for a page script; a long way short of a
  real engine, which has megabytes to spend.

[ROADMAP.md](ROADMAP.md) tracks all of it, with what is done and what is not.
